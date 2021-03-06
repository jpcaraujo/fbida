/*
 * some generic framebuffer device stuff
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <setjmp.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "vt.h"
#include "fbtools.h"
#include "logind.h"

#ifdef SYSTEM_LINUX

#include <linux/kd.h>
#include <linux/vt.h>
#include <linux/fb.h>

/* -------------------------------------------------------------------- */
/* internal variables                                                   */

static struct fb_fix_screeninfo  fb_fix;
static struct fb_var_screeninfo  fb_var;
static unsigned char             *fb_mem;
static int			 fb_mem_offset = 0;

static int                       fb;

static struct termios            term;
static struct fb_var_screeninfo  fb_ovar;
static unsigned short            ored[256], ogreen[256], oblue[256];
static struct fb_cmap            ocmap = { 0, 256, ored, ogreen, oblue };

static unsigned short p_red[256], p_green[256], p_blue[256];
static struct fb_cmap p_cmap = { 0, 256, p_red, p_green, p_blue };

/* -------------------------------------------------------------------- */
/* palette handling                                                     */

static unsigned short color_scale(int n, int max)
{
    int ret = 65535.0 * (float)n/(max);
    if (ret > 65535) ret = 65535;
    if (ret <     0) ret =     0;
    return ret;
}

static void fb_linear_palette(int r, int g, int b)
{
    int i, size;

    size = 256 >> (8 - r);
    for (i = 0; i < size; i++)
        p_red[i] = color_scale(i,size);
    p_cmap.len = size;

    size = 256 >> (8 - g);
    for (i = 0; i < size; i++)
        p_green[i] = color_scale(i,size);
    if (p_cmap.len < size)
	p_cmap.len = size;

    size = 256 >> (8 - b);
    for (i = 0; i < size; i++)
	p_blue[i] = color_scale(i,size);
    if (p_cmap.len < size)
	p_cmap.len = size;
}

static void fb_set_palette(void)
{
    if (fb_fix.visual != FB_VISUAL_DIRECTCOLOR && fb_var.bits_per_pixel != 8)
	return;
    if (-1 == ioctl(fb,FBIOPUTCMAP,&p_cmap)) {
	perror("ioctl FBIOPUTCMAP");
	exit(1);
    }
}

/* -------------------------------------------------------------------- */
/* initialisation & cleanup                                             */

static void
fb_memset (void *addr, int c, size_t len)
{
#if 1 /* defined(__powerpc__) */
    unsigned int i, *p;

    i = (c & 0xff) << 8;
    i |= i << 16;
    len >>= 2;
    for (p = addr; len--; p++)
	*p = i;
#else
    memset(addr, c, len);
#endif
}

static int
fb_setmode(char *name)
{
    FILE *fp;
    char line[80],label[32],value[16];
    int  geometry=0, timings=0;

    /* load current values */
    if (-1 == ioctl(fb,FBIOGET_VSCREENINFO,&fb_var)) {
	perror("ioctl FBIOGET_VSCREENINFO");
	exit(1);
    }

    if (NULL == name)
	return -1;
    if (NULL == (fp = fopen("/etc/fb.modes","r")))
	return -1;
    while (NULL != fgets(line,79,fp)) {
	if (1 == sscanf(line, "mode \"%31[^\"]\"",label) &&
	    0 == strcmp(label,name)) {
	    /* fill in new values */
	    fb_var.sync  = 0;
	    fb_var.vmode = 0;
	    while (NULL != fgets(line,79,fp) &&
		   NULL == strstr(line,"endmode")) {
		if (5 == sscanf(line," geometry %d %d %d %d %d",
				&fb_var.xres,&fb_var.yres,
				&fb_var.xres_virtual,&fb_var.yres_virtual,
				&fb_var.bits_per_pixel))
		    geometry = 1;
		if (7 == sscanf(line," timings %d %d %d %d %d %d %d",
				&fb_var.pixclock,
				&fb_var.left_margin,  &fb_var.right_margin,
				&fb_var.upper_margin, &fb_var.lower_margin,
				&fb_var.hsync_len,    &fb_var.vsync_len))
		    timings = 1;
		if (1 == sscanf(line, " hsync %15s",value) &&
		    0 == strcasecmp(value,"high"))
		    fb_var.sync |= FB_SYNC_HOR_HIGH_ACT;
		if (1 == sscanf(line, " vsync %15s",value) &&
		    0 == strcasecmp(value,"high"))
		    fb_var.sync |= FB_SYNC_VERT_HIGH_ACT;
		if (1 == sscanf(line, " csync %15s",value) &&
		    0 == strcasecmp(value,"high"))
		    fb_var.sync |= FB_SYNC_COMP_HIGH_ACT;
		if (1 == sscanf(line, " extsync %15s",value) &&
		    0 == strcasecmp(value,"true"))
		    fb_var.sync |= FB_SYNC_EXT;
		if (1 == sscanf(line, " laced %15s",value) &&
		    0 == strcasecmp(value,"true"))
		    fb_var.vmode |= FB_VMODE_INTERLACED;
		if (1 == sscanf(line, " double %15s",value) &&
		    0 == strcasecmp(value,"true"))
		    fb_var.vmode |= FB_VMODE_DOUBLE;
	    }
	    /* ok ? */
	    if (!geometry || !timings)
		return -1;
	    /* set */
	    fb_var.xoffset = 0;
	    fb_var.yoffset = 0;
	    if (-1 == ioctl(fb,FBIOPUT_VSCREENINFO,&fb_var))
		perror("ioctl FBIOPUT_VSCREENINFO");
	    /* look what we have now ... */
	    if (-1 == ioctl(fb,FBIOGET_VSCREENINFO,&fb_var)) {
		perror("ioctl FBIOGET_VSCREENINFO");
		exit(1);
	    }
	    return 0;
	}
    }
    fclose(fp);
    return -1;
}

static void fb_suspend_display(void)
{
}

static int fb_resume_display(void)
{
    ioctl(fb,FBIOPAN_DISPLAY,&fb_var);
    fb_set_palette();
    return 0;
}

static void fb_cleanup_display(void)
{
    /* restore console */
    if (-1 == ioctl(fb, FBIOPUT_VSCREENINFO, &fb_ovar))
	perror("ioctl FBIOPUT_VSCREENINFO");
    if (-1 == ioctl(fb, FBIOGET_FSCREENINFO, &fb_fix))
	perror("ioctl FBIOGET_FSCREENINFO");
    if (fb_ovar.bits_per_pixel == 8 ||
	fb_fix.visual == FB_VISUAL_DIRECTCOLOR) {
	if (-1 == ioctl(fb, FBIOPUTCMAP, &ocmap))
	    perror("ioctl FBIOPUTCMAP");
    }
    close(fb);

    tcsetattr(STDIN_FILENO, TCSANOW, &term);
}

/* -------------------------------------------------------------------- */

gfxstate* fb_init(const char *device, char *mode)
{
    unsigned long page_mask;
    struct stat st;
    gfxstate *gfx;
    gfxfmt *fmt = NULL;

    if (NULL == device) {
	device = getenv("FRAMEBUFFER");
	if (NULL == device) {
            device = "/dev/fb0";
        }
    }
    fprintf(stderr, "trying fbdev: %s ...\n", device);

    /* get current settings (which we have to restore) */
    if (-1 == (fb = device_open(device))) {
	fprintf(stderr,"open %s: %s\n",device,strerror(errno));
	exit(1);
    }
    if (-1 == ioctl(fb,FBIOGET_VSCREENINFO,&fb_ovar)) {
	perror("ioctl FBIOGET_VSCREENINFO");
	exit(1);
    }
    if (-1 == ioctl(fb,FBIOGET_FSCREENINFO,&fb_fix)) {
	perror("ioctl FBIOGET_FSCREENINFO");
	exit(1);
    }
    if (fb_ovar.bits_per_pixel == 8 ||
	fb_fix.visual == FB_VISUAL_DIRECTCOLOR) {
	if (-1 == ioctl(fb,FBIOGETCMAP,&ocmap)) {
	    perror("ioctl FBIOGETCMAP");
	    exit(1);
	}
    }
    tcgetattr(STDIN_FILENO, &term);

    /* switch mode */
    fb_setmode(mode);

    /* checks & initialisation */
    if (-1 == ioctl(fb,FBIOGET_FSCREENINFO,&fb_fix)) {
	perror("ioctl FBIOGET_FSCREENINFO");
	exit(1);
    }
    if (fb_fix.type != FB_TYPE_PACKED_PIXELS) {
	fprintf(stderr,"can handle only packed pixel frame buffers\n");
	goto err;
    }
    page_mask = getpagesize()-1;
    fb_mem_offset = (unsigned long)(fb_fix.smem_start) & page_mask;
    fb_mem = mmap(NULL,fb_fix.smem_len+fb_mem_offset,
		  PROT_READ|PROT_WRITE,MAP_SHARED,fb,0);
    if (-1L == (long)fb_mem) {
	perror("mmap");
	goto err;
    }
    /* move viewport to upper left corner */
    if (fb_var.xoffset != 0 || fb_var.yoffset != 0) {
	fb_var.xoffset = 0;
	fb_var.yoffset = 0;
	if (-1 == ioctl(fb,FBIOPAN_DISPLAY,&fb_var)) {
	    perror("ioctl FBIOPAN_DISPLAY");
	    goto err;
	}
    }
#if 0
    console_activate_current();
#endif

    /* cls */
    fb_memset(fb_mem+fb_mem_offset, 0, fb_fix.line_length * fb_var.yres);

    /* init palette */
    switch (fb_var.bits_per_pixel) {
    case 15:
        fmt = gfx_fmt_find_pixman(PIXMAN_x1r5g5b5);
        fb_linear_palette(5,5,5);
        break;
    case 16:
        fmt = gfx_fmt_find_pixman(PIXMAN_r5g6b5);
        fb_linear_palette(5,6,5);
	break;
    case 32:
        fmt = gfx_fmt_find_pixman(PIXMAN_x8r8g8b8);
        fb_linear_palette(8,8,8);
	break;
    }
    fb_set_palette();

    if (!fmt) {
        fprintf(stderr,"can't handle framebuffer format\n");
        goto err;
    }

    /* prepare gfx */
    gfx = malloc(sizeof(*gfx));
    memset(gfx, 0, sizeof(*gfx));

    gfx->hdisplay        = fb_var.xres;
    gfx->vdisplay        = fb_var.yres;
    gfx->stride          = fb_fix.line_length;
    gfx->mem             = fb_mem;
    gfx->fmt             = fmt;

    gfx->suspend_display = fb_suspend_display;
    gfx->resume_display  = fb_resume_display;
    gfx->cleanup_display = fb_cleanup_display;

    fstat(fb, &st);
    gfx->devnum  = st.st_rdev;
    snprintf(gfx->devpath, sizeof(gfx->devpath), "%s", device);

    return gfx;

 err:
    fb_cleanup_display();
    exit(1);
}

#else /* SYSTEM_LINUX */

gfxstate* fb_init(const char *device, char *mode)
{
    return NULL;
}
#endif
