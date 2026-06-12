// SPDX-License-Identifier: GPL-2.0
/*
 * dfr-switch — atomic T1 display-mode entry experiment.
 *
 * Unlike dfr-spike.c (which assumed the device was already in config 2 and was
 * fed by a sysfs `echo 2` that wedged the firmware), this does the WHOLE
 * transition itself, in one process, with no gap where the firmware sits in
 * display mode unattended:
 *
 *   1. detach kernel drivers from all current interfaces
 *   2. libusb_set_configuration(2)            <- and report the exact return code
 *   3. claim the Audio/Video interface (3), find its bulk endpoints
 *   4. GINF -> REDY -> (optional) paint one frame   <- immediately, before timeout
 *   5. restore config 1 + reattach kernel drivers   (best-effort; reboot if it sulks)
 *
 * This is a dedicated experimentation machine; a wedge costs a reboot, which is
 * acceptable. Run as root.  Build: gcc -O2 -Wall -o dfr-switch dfr-switch.c -lusb-1.0
 * Usage: sudo ./dfr-switch info            (switch, GINF, restore — proves entry+protocol)
 *        sudo ./dfr-switch frame [RRGGBB]  (switch, GINF, REDY, paint, restore)
 *        sudo ./dfr-switch hold            (switch + GINF + REDY, then SLEEP holding config 2
 *                                           so you can inspect lsusb/dmesg; Ctrl-C to restore)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <libusb-1.0/libusb.h>

#define VID 0x05ac
#define PID 0x8600
#define CFG_DISPLAY 2
#define IFACE_AV 3
#define TIMEOUT_MS 2000
#define BIG_TIMEOUT_MS 6000
/* config-2 HID interfaces (touch digitizer lives on one of these interrupt EPs) */
#define HID_IFACE_A 2
#define HID_IFACE_B 6
#define EP_TOUCH_A 0x83
#define EP_TOUCH_B 0x87

#define MSG_CLRD 0x434c5244u
#define MSG_GINF 0x47494e46u
#define MSG_UDCL 0x5544434cu
#define MSG_REDY 0x52454459u
#define PIXFMT   0x52474241u
#define BPP_EXPECTED 24

#pragma pack(push, 1)
struct req_header { uint16_t unk00, unk02; uint32_t unk04, unk08, size; };
struct simple_request { struct req_header h; uint32_t msg; uint8_t unk14[8]; uint32_t size; };
struct resp_header { uint8_t unk00[16]; uint32_t msg; };
struct info_resp { struct resp_header h; uint8_t unk14[12]; uint32_t width, height; uint8_t bpp;
                   uint32_t bytes_per_row, orientation, bitmap_info, pixel_format, w_in, h_in; };
struct frame_hdr { uint16_t begin_x, begin_y, width, height; uint32_t buf_size; };
struct fb_footer { uint8_t a[12]; uint32_t unk0c; uint8_t b[12]; uint32_t unk1c; uint64_t timestamp;
                   uint8_t c[12]; uint32_t unk34; uint8_t d[20]; uint32_t unk4c; };
struct fb_req_hdr { struct req_header h; uint16_t unk10; uint8_t msg_id; uint8_t unk13[29]; };
struct fb_resp { struct resp_header h; uint8_t unk14[12]; uint64_t timestamp; };
#pragma pack(pop)

static libusb_device_handle *dev;
static uint8_t ep_out = 0x02, ep_in = 0x85;
static volatile sig_atomic_t stop;
static void on_int(int s){ (void)s; stop = 1; }

/* Temporarily stop the kernel from auto-binding drivers to USB interfaces, so it
 * can't bind (and reset the device off) the config-2 interfaces while we work. */
static void set_autoprobe(int v){
	FILE*f=fopen("/sys/bus/usb/drivers_autoprobe","w");
	if(f){ fprintf(f,"%d\n",v); fclose(f); printf("drivers_autoprobe=%d\n",v); }
	else printf("(could not set drivers_autoprobe)\n");
}

static uint64_t now_ns(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
static void fourcc(uint32_t v,char o[5]){ o[0]=v; o[1]=v>>8; o[2]=v>>16; o[3]=v>>24; o[4]=0; }

static int bulk_out(const void*b,int n){ int t=0,to=n>4096?BIG_TIMEOUT_MS:TIMEOUT_MS;
	int r=libusb_bulk_transfer(dev,ep_out,(unsigned char*)b,n,&t,to);
	if(r){ fprintf(stderr,"  bulk OUT %s\n",libusb_error_name(r)); return r; }
	if(t!=n){ fprintf(stderr,"  bulk OUT short %d/%d\n",t,n); return -1; } return 0; }

static int send_msg(uint32_t msg){ struct simple_request rq; memset(&rq,0,sizeof rq);
	rq.h.unk00=2; rq.h.unk02=0x1512; rq.h.size=sizeof rq-sizeof rq.h; rq.msg=msg; rq.size=rq.h.size;
	return bulk_out(&rq,sizeof rq); }

static int read_resp(void*buf,int len,uint32_t expected){ int t,tries=0; char a[5],b[5];
retry:
	t=0; { int r=libusb_bulk_transfer(dev,ep_in,(unsigned char*)buf,len,&t,TIMEOUT_MS);
	if(r){ fprintf(stderr,"  bulk IN %s\n",libusb_error_name(r)); return r; } }
	uint32_t msg; memcpy(&msg,(uint8_t*)buf+offsetof(struct resp_header,msg),4);
	if(msg==MSG_REDY){ if(tries++==0){ fprintf(stderr,"  (REDY readiness, re-reading)\n"); goto retry; }
		fprintf(stderr,"  second REDY\n"); return -1; }
	if(msg!=expected){ fourcc(expected,a); fourcc(msg,b);
		fprintf(stderr,"  expected %s got %s (size %d)\n",a,b,t); return -1; }
	if(t!=len){ fprintf(stderr,"  short %d/%d\n",t,len); return -1; } return 0; }

static int do_info(struct info_resp*info){ char fc[5];
	if(send_msg(MSG_GINF)) return -1;
	if(read_resp(info,sizeof*info,MSG_GINF)) return -1;
	fourcc(info->pixel_format,fc);
	printf("GINF: %ux%u bpp=%u bytes_per_row=%u orientation=%u pixfmt=%s(0x%08x)\n",
	       info->width,info->height,info->bpp,info->bytes_per_row,info->orientation,fc,info->pixel_format);
	if(info->bpp==BPP_EXPECTED && info->pixel_format==PIXFMT)
		printf("  => T1 ENTERED DISPLAY MODE and speaks appletbdrm. Custom rendering reachable.\n");
	else
		printf("  WARNING: unexpected bpp/pixfmt (expected 24 / 0x%08x)\n",PIXFMT);
	return 0; }

static int do_frame(struct info_resp*info,uint8_t R,uint8_t G,uint8_t B){
	uint32_t w=info->width,h=info->height,bs=w*h*3;
	size_t frames=sizeof(struct frame_hdr)+bs;
	size_t rs=(sizeof(struct fb_req_hdr)+frames+sizeof(struct fb_footer)+15)&~(size_t)15;
	uint8_t*req=calloc(1,rs); if(!req){ fprintf(stderr,"OOM\n"); return -1; }
	uint64_t ts=now_ns();
	struct fb_req_hdr*hd=(void*)req;
	hd->h.unk00=2; hd->h.unk02=0x12; hd->h.unk04=9; hd->h.size=rs-sizeof(struct req_header);
	hd->unk10=1; hd->msg_id=(uint8_t)ts;
	struct frame_hdr*fr=(void*)(req+sizeof(struct fb_req_hdr));
	fr->begin_x=0; fr->begin_y=0; fr->width=w; fr->height=h; fr->buf_size=bs;
	uint8_t*px=(uint8_t*)fr+sizeof(struct frame_hdr);
	for(uint32_t i=0;i<w*h;i++){ px[3*i]=R; px[3*i+1]=G; px[3*i+2]=B; } /* T1 panel byte order is R,G,B */
	struct fb_footer*ft=(void*)(req+sizeof(struct fb_req_hdr)+frames);
	ft->unk0c=0xfffe; ft->unk1c=0x80001; ft->unk34=0x80002; ft->unk4c=0xffff; ft->timestamp=ts;
	printf("frame: %ux%u solid R=%u G=%u B=%u (%zu bytes)\n",w,h,R,G,B,rs);
	if(bulk_out(req,rs)){ free(req); return -1; }
	/* T1 returns a SHORTER completion ack than T2's 40-byte UDCL — read raw and
	 * dump it so we can pin the format for the kernel appletbdrm port. */
	uint8_t ack[64]; int an=0;
	int rr=libusb_bulk_transfer(dev,ep_in,ack,sizeof ack,&an,TIMEOUT_MS);
	printf("  FRAME ACCEPTED. ack: %s, %d bytes:",rr?libusb_error_name(rr):"ok",an);
	for(int i=0;i<an && i<32;i++) printf(" %02x",ack[i]);
	printf("\n");
	free(req); return 0; }

static int do_gradient(struct info_resp*info){
	uint32_t w=info->width,h=info->height,bs=w*h*3;
	size_t frames=sizeof(struct frame_hdr)+bs;
	size_t rs=(sizeof(struct fb_req_hdr)+frames+sizeof(struct fb_footer)+15)&~(size_t)15;
	uint8_t*req=calloc(1,rs); if(!req){ fprintf(stderr,"OOM\n"); return -1; }
	uint64_t ts=now_ns();
	struct fb_req_hdr*hd=(void*)req;
	hd->h.unk00=2; hd->h.unk02=0x12; hd->h.unk04=9; hd->h.size=rs-sizeof(struct req_header);
	hd->unk10=1; hd->msg_id=(uint8_t)ts;
	struct frame_hdr*fr=(void*)(req+sizeof(struct fb_req_hdr));
	fr->begin_x=0; fr->begin_y=0; fr->width=w; fr->height=h; fr->buf_size=bs;
	uint8_t*px=(uint8_t*)fr+sizeof(struct frame_hdr);
	/* buffer is W rows of H px (the panel is rotated 90°): long axis = i/H */
	for(uint32_t i=0;i<w*h;i++){ uint32_t a=i/h; uint8_t v=(uint8_t)(255u*a/w);
		px[3*i]=v; px[3*i+1]=0; px[3*i+2]=(uint8_t)(255-v); }   /* blue→magenta sweep along the bar */
	struct fb_footer*ft=(void*)(req+sizeof(struct fb_req_hdr)+frames);
	ft->unk0c=0xfffe; ft->unk1c=0x80001; ft->unk34=0x80002; ft->unk4c=0xffff; ft->timestamp=ts;
	printf("frame: %ux%u gradient (%zu bytes)\n",w,h,rs);
	if(bulk_out(req,rs)){ free(req); return -1; }
	uint8_t ack[64]; int an=0;
	libusb_bulk_transfer(dev,ep_in,ack,sizeof ack,&an,TIMEOUT_MS);
	printf("  gradient ACCEPTED (ack %d bytes)\n",an);
	free(req); return 0; }

/* draw N solid colour zones across the bar (the "buttons") */
static int do_bands(struct info_resp*info,int N){
	uint32_t w=info->width,h=info->height,bs=w*h*3;
	size_t frames=sizeof(struct frame_hdr)+bs;
	size_t rs=(sizeof(struct fb_req_hdr)+frames+sizeof(struct fb_footer)+15)&~(size_t)15;
	uint8_t*req=calloc(1,rs); if(!req){ fprintf(stderr,"OOM\n"); return -1; }
	uint64_t ts=now_ns();
	struct fb_req_hdr*hd=(void*)req;
	hd->h.unk00=2; hd->h.unk02=0x12; hd->h.unk04=9; hd->h.size=rs-sizeof(struct req_header);
	hd->unk10=1; hd->msg_id=(uint8_t)ts;
	struct frame_hdr*fr=(void*)(req+sizeof(struct fb_req_hdr));
	fr->begin_x=0; fr->begin_y=0; fr->width=w; fr->height=h; fr->buf_size=bs;
	uint8_t*px=(uint8_t*)fr+sizeof(struct frame_hdr);
	static const uint8_t col[4][3]={{255,0,0},{0,255,0},{0,0,255},{255,255,0}}; /* RGB: red,green,blue,yellow */
	/* buffer is W rows of H px (panel rotated 90°): long axis = i/H, not i%W */
	for(uint32_t i=0;i<w*h;i++){ uint32_t a=i/h; int z=(int)((uint64_t)a*N/w); if(z>=N)z=N-1; if(z<0)z=0;
		px[3*i]=col[z&3][0]; px[3*i+1]=col[z&3][1]; px[3*i+2]=col[z&3][2]; }
	struct fb_footer*ft=(void*)(req+sizeof(struct fb_req_hdr)+frames);
	ft->unk0c=0xfffe; ft->unk1c=0x80001; ft->unk34=0x80002; ft->unk4c=0xffff; ft->timestamp=ts;
	if(bulk_out(req,rs)){ free(req); return -1; }
	uint8_t ack[64]; int an=0; libusb_bulk_transfer(dev,ep_in,ack,sizeof ack,&an,TIMEOUT_MS);
	printf("drew %d colour zones (red|green|blue|yellow across the bar)\n",N);
	free(req); return 0; }

/* uinput virtual keyboard so taps become real keystrokes */
static int ui=-1;
static const int ZONE_KEYS[4]={KEY_ESC,KEY_A,KEY_B,KEY_C};
static const char*ZONE_NAME[4]={"ESC","A","B","C"};
static int uinput_open(void){
	ui=open("/dev/uinput",O_WRONLY|O_NONBLOCK);
	if(ui<0){ perror("open /dev/uinput"); return -1; }
	ioctl(ui,UI_SET_EVBIT,EV_KEY);
	for(int i=0;i<4;i++) ioctl(ui,UI_SET_KEYBIT,ZONE_KEYS[i]);
	struct uinput_setup us; memset(&us,0,sizeof us);
	us.id.bustype=BUS_USB; us.id.vendor=0x1209; us.id.product=0x7401;
	strcpy(us.name,"t1-touchbar-buttons");
	if(ioctl(ui,UI_DEV_SETUP,&us)<0||ioctl(ui,UI_DEV_CREATE)<0){ perror("uinput create"); close(ui); ui=-1; return -1; }
	usleep(100000); return 0; }
static void emit(int t,int c,int v){ struct input_event e; memset(&e,0,sizeof e); e.type=t; e.code=c; e.value=v; if(write(ui,&e,sizeof e)<0){} }
static void tap_key(int k){ emit(EV_KEY,k,1); emit(EV_SYN,SYN_REPORT,0); emit(EV_KEY,k,0); emit(EV_SYN,SYN_REPORT,0); }

int main(int argc,char**argv){
	const char*stage=argc>1?argv[1]:"info";
	int rc=1, switched=0;
	signal(SIGINT,on_int); signal(SIGTERM,on_int);

	if(libusb_init(NULL)){ fprintf(stderr,"libusb_init failed\n"); return 1; }
	dev=libusb_open_device_with_vid_pid(NULL,VID,PID);
	if(!dev){ fprintf(stderr,"device %04x:%04x not found\n",VID,PID); goto out; }

	int cfg=-1; libusb_get_configuration(dev,&cfg);
	printf("start config=%d\n",cfg);

	/* Stop the kernel auto-binding drivers to the config-2 interfaces (which
	 * resets us back to config 1). Restored at exit. */
	set_autoprobe(0);

	printf("detaching kernel drivers from interfaces 0..7...\n");
	for(int i=0;i<8;i++){ int a=libusb_kernel_driver_active(dev,i);
		if(a==1){ int r=libusb_detach_kernel_driver(dev,i);
			printf("  intf %d: detached (%s)\n",i,r?libusb_error_name(r):"ok"); }
	}

	/* Go UNCONFIGURED first, so config 2 is a fresh SET_CONFIGURATION from
	 * config 0 (like enumeration) rather than a live 1->2 switch (which the
	 * firmware sometimes answers with a USB reset back to config 1). Retry,
	 * since reaching config 2 is mildly racy. */
	/* Config 2 is only briefly active: the kernel reverts it to config 1 ~45ms
	 * after we select it (firmware times out / a driver probe resets it). The
	 * fix is to CLAIM the AV interface (which exists ONLY in config 2) in that
	 * window — a held claim both proves we're in display mode AND blocks the
	 * kernel from changing config. So: select config 2, then IMMEDIATELY (no
	 * round-trips) tight-loop the claim. Retry the whole thing until we win. */
	int r=0, got=0, cr=LIBUSB_ERROR_BUSY;
	for(int attempt=1; attempt<=20 && !got; attempt++){
		for(int i=0;i<8;i++) if(libusb_kernel_driver_active(dev,i)==1) libusb_detach_kernel_driver(dev,i);
		libusb_set_configuration(dev,-1);
		usleep(120000);
		r=libusb_set_configuration(dev,CFG_DISPLAY);
		/* Only accept the claim while config==2, so we don't grab config-1's
		 * interface 3 (the HID one) after a revert. With autoprobe off config 2
		 * should simply hold. */
		cr=LIBUSB_ERROR_BUSY;
		for(int t=0; t<100 && cr; t++){
			libusb_get_configuration(dev,&cfg);
			if(cfg!=CFG_DISPLAY){ usleep(2000); continue; }
			libusb_detach_kernel_driver(dev,IFACE_AV);   /* ignore errors */
			cr=libusb_claim_interface(dev,IFACE_AV);
			if(cr) usleep(2000);
		}
		libusb_get_configuration(dev,&cfg);
		printf("[entry %2d] set_cfg(2)->%-18s claim intf%d->%-10s config=%d\n",
		       attempt, r?libusb_error_name(r):"ok", IFACE_AV, cr?libusb_error_name(cr):"CLAIMED", cfg);
		if(cr==0 && cfg==CFG_DISPLAY){ got=1; switched=1; break; }
		if(cr==0){ libusb_release_interface(dev,IFACE_AV); cr=-1; }  /* grabbed wrong config's intf3 */
		usleep(150000);
	}
	if(!got){ fprintf(stderr,"could not grab AV interface in config 2 after retries. Restoring.\n"); goto restore; }
	printf("** CLAIMED AV interface %d, config locked at %d (bulk OUT 0x%02x / IN 0x%02x) **\n",
	       IFACE_AV,cfg,ep_out,ep_in);

	/* mirror the Windows driver: reset/clear both bulk pipes before GINF */
	libusb_clear_halt(dev,ep_out);
	libusb_clear_halt(dev,ep_in);

	struct info_resp info; memset(&info,0,sizeof info);
	if(do_info(&info)) goto release;

	if(!strcmp(stage,"info")){ rc=0; }
	else if(!strcmp(stage,"hold")){
		if(send_msg(MSG_REDY)==0){ printf("REDY sent. Holding config 2 — inspect with `lsusb -v`/`dmesg`. Ctrl-C to restore.\n");
			while(!stop) sleep(1); rc=0; } }
	else if(!strcmp(stage,"frame")){
		uint32_t rgb=argc>2?(uint32_t)strtoul(argv[2],NULL,16):0xFF00FF;
		if(send_msg(MSG_REDY)==0) rc=do_frame(&info,(rgb>>16)&0xff,(rgb>>8)&0xff,rgb&0xff);
	}
	else if(!strcmp(stage,"demo")){
		if(send_msg(MSG_REDY)==0){
			do_frame(&info,255,0,0);   usleep(500000);
			do_frame(&info,0,255,0);   usleep(500000);
			do_frame(&info,0,0,255);   usleep(500000);
			do_frame(&info,255,255,255);usleep(500000);
			do_gradient(&info);        usleep(900000);
			rc=0;
		}
	}
	else if(!strcmp(stage,"touch")){
		/* Display mode active; now CAPTURE the raw touch digitizer. In config 2
		 * the firmware streams multitouch coordinates over a HID interrupt EP
		 * instead of acting as the brightness/volume control strip. Dump reports
		 * so we can see the coordinate format and prove the input side. */
		if(send_msg(MSG_REDY)==0){
			do_frame(&info,255,0,255);   /* show magenta so you can see where you tap */
			libusb_detach_kernel_driver(dev,HID_IFACE_B);
			int hb=libusb_claim_interface(dev,HID_IFACE_B);
			libusb_detach_kernel_driver(dev,HID_IFACE_A);
			int ha=libusb_claim_interface(dev,HID_IFACE_A);
			printf("claimed HID intf %d->%s, %d->%s\n",
			       HID_IFACE_B,hb?libusb_error_name(hb):"ok",HID_IFACE_A,ha?libusb_error_name(ha):"ok");
			printf(">>> TOUCH THE BAR now. Each report prints below. Ctrl-C to stop. <<<\n");
			uint8_t buf[1024]; int n;
			while(!stop){
				if(hb==0){ n=0;
					if(libusb_interrupt_transfer(dev,EP_TOUCH_B,buf,sizeof buf,&n,200)==0 && n>0){
						printf("touch[ep%02x] %2d:",EP_TOUCH_B,n); for(int i=0;i<n&&i<28;i++)printf(" %02x",buf[i]); printf("\n"); continue; } }
				if(ha==0){ n=0;
					if(libusb_interrupt_transfer(dev,EP_TOUCH_A,buf,sizeof buf,&n,200)==0 && n>0){
						printf("touch[ep%02x] %2d:",EP_TOUCH_A,n); for(int i=0;i<n&&i<28;i++)printf(" %02x",buf[i]); printf("\n"); } }
			}
			if(ha==0) libusb_release_interface(dev,HID_IFACE_A);
			if(hb==0) libusb_release_interface(dev,HID_IFACE_B);
			rc=0;
		}
	}
	else if(!strcmp(stage,"buttons")){
		/* THE END-TO-END DEMO: draw 4 zones, read touch X, inject a real key per zone. */
		if(send_msg(MSG_REDY)==0){
			do_bands(&info,4);
			libusb_detach_kernel_driver(dev,HID_IFACE_A);
			int ha=libusb_claim_interface(dev,HID_IFACE_A);
			printf("claimed touch intf %d -> %s\n",HID_IFACE_A,ha?libusb_error_name(ha):"ok");
			if(ha==0 && uinput_open()==0){
				printf(">>> TAP the coloured zones. red=ESC green=A blue=B yellow=C. Ctrl-C to stop. <<<\n");
				uint8_t buf[64]; int n, down=0;
				while(!stop){
					n=0;
					int r=libusb_interrupt_transfer(dev,EP_TOUCH_A,buf,sizeof buf,&n,120);
					if(r==0 && n>=4){
						float x; memcpy(&x,buf,4);
						float nx=(x-0.5f)*2.0f; if(nx<0)nx=0; if(nx>0.9999f)nx=0.9999f; /* touch X is [0.5,1.0] */
						int z=(int)(nx*4.0f); if(z<0)z=0; if(z>3)z=3;
						if(!down){ down=1; tap_key(ZONE_KEYS[z]);
							printf("TAP x=%.3f (nx=%.3f) -> zone %d -> key %s\n",x,nx,z,ZONE_NAME[z]); fflush(stdout); }
					} else if(r==LIBUSB_ERROR_TIMEOUT){ down=0; }
				}
				ioctl(ui,UI_DEV_DESTROY); close(ui);
			}
			if(ha==0) libusb_release_interface(dev,HID_IFACE_A);
			rc=0;
		}
	}
	else fprintf(stderr,"unknown stage '%s' (info|hold|frame|demo|touch|buttons)\n",stage);

release:
	libusb_release_interface(dev,IFACE_AV);
restore:
	if(switched){ printf("restoring config 1...\n");
		for(int i=0;i<8;i++) if(libusb_kernel_driver_active(dev,i)==1) libusb_detach_kernel_driver(dev,i);
		libusb_set_configuration(dev,-1); usleep(150000);
		int r2=libusb_set_configuration(dev,1);
		printf("  set_configuration(1) -> %d (%s)\n",r2,r2?libusb_error_name(r2):"ok");
	}
	set_autoprobe(1);
	printf("reattaching kernel drivers...\n");
	for(int i=0;i<8;i++) libusb_attach_kernel_driver(dev,i);
	printf("done. If the Touch Bar is dark, run: sudo /usr/local/sbin/touchbar-relight-reload  (else reboot)\n");
	libusb_close(dev);
out:
	libusb_exit(NULL);
	return rc;
}
