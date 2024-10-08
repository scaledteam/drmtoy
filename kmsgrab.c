#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include <xf86drm.h>
#include <libdrm/drm_fourcc.h>
#include <xf86drmMode.h>

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>

#include <X11/Xatom.h>
#include <string.h>


#define MSG(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)

#define ASSERT(cond) \
	if (!(cond)) { \
		MSG("ERROR @ %s:%d: (%s) failed", __FILE__, __LINE__, #cond); \
		return 0; \
	}


typedef struct {
	int width, height;
	uint32_t fourcc;
	int fd, offset, pitch;
} DmaBuf;

uint32_t lastGoodPlane = 0;

uint32_t prepareImage(const int fd, int cursor) {
	
	drmModePlaneResPtr planes = drmModeGetPlaneResources(fd);
	
	// Check the first plane (or last good)
	drmModePlanePtr plane = drmModeGetPlane(fd, planes->planes[lastGoodPlane]);
	uint32_t fb_id = plane->fb_id;
	drmModeFreePlane(plane);
	
	// Find a good plane
	if (fb_id == 0) {
		for (uint32_t i = 0; i < planes->count_planes; ++i) {
			drmModePlanePtr plane = drmModeGetPlane(fd, planes->planes[i]);
			
			if (plane->fb_id != 0) {
				drmModeFBPtr fb = drmModeGetFB(fd, plane->fb_id);
				if (fb == NULL) {
					//ctx->lastGoodPlane = 0;
					continue;
				}
				if (fb->handle) {
					// most likely cursor	
					//MSG("%d\t%d\n", fb->width, fb->height);
					if (cursor) {
						if (fb->width != 256 && fb->height != 256)
							continue;
					}
					else {
						if (fb->width == 256 && fb->height == 256)
							continue;
					}
				}
				drmModeFreeFB(fb);
				
				lastGoodPlane = i;
				fb_id = plane->fb_id;
				//MSG("%d, %#x", i, fb_id);
				
				drmModeFreePlane(plane);
				break;
			}
			else {
				drmModeFreePlane(plane);
			}
		}
	}
	
	drmModeFreePlaneResources(planes);
	
	//MSG("%#x", fb_id);
	return fb_id;
}

//static int width = 1280, height = 720;
int main(int argc, const char *argv[]) {
	//const char *card = (argc > 2) ? argv[2] : "/dev/dri/card0";
	const char *card = "/dev/dri/card0";
	
	int cursor = 0;
	int width = 1280;
	int height = 720;
	int fullscreen = 0;
	//const char *window_name = (argc > 3) ? argv[3] : "kmsgrab";
	
	if (argc == 2) {
		sscanf (argv[1], "%i", &width);
		if (width == -1)
			fullscreen = 1;
		width = 1280;
	}
	
	if (argc > 2) {
		sscanf (argv[1], "%i", &width);
		sscanf (argv[2], "%i", &height);
	}

	MSG("Opening card %s", card);
	const int drmfd = open(card, O_RDONLY);
	if (drmfd < 0) {
		perror("Cannot open card");
		return 1;
	}
	drmSetClientCap(drmfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	
	const int available = drmAvailable();
	if (!available)
		return 0;
	
	
	// Find DRM video source
	uint32_t fb_id = prepareImage(drmfd, cursor);

	if (fb_id == 0) {
		MSG("Not found fb_id");
		return 1;
	}

	int dma_buf_fd = -1;
	drmModeFBPtr fb = drmModeGetFB(drmfd, fb_id);
	if (!fb->handle) {
		MSG("Not permitted to get fb handles. Run either with sudo, or setcap cap_sys_admin+ep %s", argv[0]);
		
		if (dma_buf_fd >= 0)
			close(dma_buf_fd);
		if (fb)
			drmModeFreeFB(fb);
		close(drmfd);
		return 0;
	}

	DmaBuf img;
	img.width = fb->width;
	img.height = fb->height;
	img.pitch = fb->pitch;
	img.offset = 0;
	img.fourcc = DRM_FORMAT_XRGB8888; // FIXME
	drmPrimeHandleToFD(drmfd, fb->handle, 0, &dma_buf_fd);
	img.fd = dma_buf_fd;


	
	// render all
	Display *xdisp;
	ASSERT(xdisp = XOpenDisplay(NULL));
	eglBindAPI(EGL_OPENGL_API);
	EGLDisplay edisp = eglGetDisplay(xdisp);
	EGLint ver_min, ver_maj;
	eglInitialize(edisp, &ver_maj, &ver_min);
	/*MSG("EGL: version %d.%d", ver_maj, ver_min);
	MSG("EGL: EGL_VERSION: '%s'", eglQueryString(edisp, EGL_VERSION));
	MSG("EGL: EGL_VENDOR: '%s'", eglQueryString(edisp, EGL_VENDOR));
	MSG("EGL: EGL_CLIENT_APIS: '%s'", eglQueryString(edisp, EGL_CLIENT_APIS));
	MSG("EGL: client EGL_EXTENSIONS: '%s'", eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS));
	MSG("EGL: EGL_EXTENSIONS: '%s'", eglQueryString(edisp, EGL_EXTENSIONS));*/

	static const EGLint econfattrs[] = {
		EGL_BUFFER_SIZE, 32,
		EGL_RED_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_ALPHA_SIZE, 8,

		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,

		EGL_NONE
	};
	EGLConfig config;
	EGLint num_config;
	eglChooseConfig(edisp, econfattrs, &config, 1, &num_config);

	XVisualInfo *vinfo = NULL;
	{
		XVisualInfo xvisual_info = {0};
		int num_visuals;
		ASSERT(eglGetConfigAttrib(edisp, config, EGL_NATIVE_VISUAL_ID, (EGLint*)&xvisual_info.visualid));
		ASSERT(vinfo = XGetVisualInfo(xdisp, VisualScreenMask | VisualIDMask, &xvisual_info, &num_visuals));
	}

	XSetWindowAttributes winattrs = {0};
	winattrs.event_mask = KeyPressMask | KeyReleaseMask |
		ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
		ExposureMask | VisibilityChangeMask | StructureNotifyMask;
	winattrs.border_pixel = 0;
	winattrs.bit_gravity = StaticGravity;
	winattrs.colormap = XCreateColormap(xdisp,
		RootWindow(xdisp, vinfo->screen),
		vinfo->visual, AllocNone);
	ASSERT(winattrs.colormap != None);
	winattrs.override_redirect = False;

	Window xwin = XCreateWindow(xdisp, RootWindow(xdisp, vinfo->screen),
		0, 0, width, height,
		0, vinfo->depth, InputOutput, vinfo->visual,
		CWBorderPixel | CWBitGravity | CWEventMask | CWColormap,
		&winattrs);
	ASSERT(xwin);
	
	/*XSizeHints* sh = XAllocSizeHints();
	sh->flags = PMinSize | PMaxSize;
	sh->min_width = sh->max_width = width;
	sh->min_height = sh->max_height = height;
	XSetWMSizeHints(xdisp, xwin, sh, XA_WM_NORMAL_HINTS);
	XFree(sh);*/
	
	/*XSizeHints* sh = XAllocSizeHints();
	sh->flags = PPosition;
	sh->x = 1222;
	sh->y = 1222;
	XSetWMSizeHints(xdisp, xwin, sh, XA_WM_NORMAL_HINTS);
	XFree(sh);*/
	
	// borderless
	/*long hints[5] = {2, 0, 0, 0, 0};
	Atom motif_hints = XInternAtom(xdisp, "_MOTIF_WM_HINTS", False);

	XChangeProperty(xdisp, xwin, motif_hints, motif_hints, 32, PropModeReplace, (unsigned char *)&hints, 5);*/
	
	// class window
	XClassHint* class_hints = XAllocClassHint();
	class_hints->res_name = (char*)malloc(sizeof(char)*8);
	class_hints->res_class = (char*)malloc(sizeof(char)*8);
	strcpy(class_hints->res_name, "kmsgrab");
	strcpy(class_hints->res_class, "kmsgrab");
	XSetClassHint(xdisp, xwin, class_hints);

	
	if (fullscreen) {
		Atom wm_state   = XInternAtom (xdisp, "_NET_WM_STATE", true );
		Atom wm_fullscreen = XInternAtom (xdisp, "_NET_WM_STATE_FULLSCREEN", true );
		XChangeProperty(xdisp, xwin, wm_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)&wm_fullscreen, 1);
	}

	XStoreName(xdisp, xwin, "kmsgrab");

	{
		Atom delete_message = XInternAtom(xdisp, "WM_DELETE_WINDOW", True);
		XSetWMProtocols(xdisp, xwin, &delete_message, 1);
	}

	XMapWindow(xdisp, xwin);

	static const EGLint ectx_attrs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	EGLContext ectx = eglCreateContext(edisp, config,
		EGL_NO_CONTEXT, ectx_attrs);
	ASSERT(EGL_NO_CONTEXT != ectx);

	EGLSurface esurf = eglCreateWindowSurface(edisp, config, xwin, 0);
	ASSERT(EGL_NO_SURFACE != esurf);

	ASSERT(eglMakeCurrent(edisp, esurf,
		esurf, ectx));
	// Set half framerate
	//eglSwapInterval(edisp, 2);

	//MSG("%s", glGetString(GL_EXTENSIONS));

	// FIXME check for EGL_EXT_image_dma_buf_import
	EGLAttrib eimg_attrs[] = {
		EGL_WIDTH, img.width,
		EGL_HEIGHT, img.height,
		EGL_LINUX_DRM_FOURCC_EXT, img.fourcc,
		EGL_DMA_BUF_PLANE0_FD_EXT, img.fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, img.offset,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, img.pitch,
		EGL_NONE
	};
	EGLImage eimg = eglCreateImage(edisp, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, 0,
		eimg_attrs);
	ASSERT(eimg);

	// FIXME check for GL_OES_EGL_image (or alternatives)
	GLuint tex = 1;
	//glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES =
		(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	ASSERT(glEGLImageTargetTexture2DOES);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, eimg);
	ASSERT(glGetError() == 0);
	//glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

	const char *fragment =
		"#version 130\n"
		"uniform vec2 res;\n"
		"uniform sampler2D tex;\n"
		"void main() {\n"
			"vec2 uv = gl_FragCoord.xy / res;\n"
			"uv.y = 1. - uv.y;\n"
			"gl_FragColor = texture(tex, uv);\n"
		"}\n"
	;
	int prog = ((PFNGLCREATESHADERPROGRAMVPROC)(eglGetProcAddress("glCreateShaderProgramv")))(GL_FRAGMENT_SHADER, 1, &fragment);
	glUseProgram(prog);
	glUniform1i(glGetUniformLocation(prog, "tex"), 0);

	for (;;) {
		while (XPending(xdisp)) {
			XEvent e;
			XNextEvent(xdisp, &e);
			switch (e.type) {
				case ConfigureNotify:
					{
						width = e.xconfigure.width;
						height = e.xconfigure.height;
					}
					break;

				case KeyPress:
					switch(XLookupKeysym(&e.xkey, 0)) {
						case XK_Escape:
						case XK_q:
							goto exit;
							break;
					}
					break;

				case ClientMessage:
				case DestroyNotify:
				case UnmapNotify:
					goto exit;
					break;
			}
		}

		{

	
			// Find DRM video source
			uint32_t fb_id = prepareImage(drmfd, cursor);

			if (fb_id == 0) {
				MSG("Not found fb_id");
			}
			else {
				if (dma_buf_fd >= 0)
					close(dma_buf_fd);
				if (fb)
					drmModeFreeFB(fb);
					
				fb = drmModeGetFB(drmfd, fb_id);
				if (!fb->handle) {
					MSG("Not permitted to get fb handles. Run either with sudo, or setcap cap_sys_admin+ep %s", argv[0]);
					
					if (fb)
						drmModeFreeFB(fb);
					close(drmfd);
					return 0;
				}

				/*img.width = fb->width;
				img.height = fb->height;
				img.pitch = fb->pitch;
				img.offset = 0;
				img.fourcc = DRM_FORMAT_XRGB8888; // FIXME*/
				drmPrimeHandleToFD(drmfd, fb->handle, 0, &dma_buf_fd);
				//img.fd = dma_buf_fd;
				
				eglDestroyImage(edisp, eimg);
				/*EGLAttrib eimg_attrs[] = {
					EGL_WIDTH, img.width,
					EGL_HEIGHT, img.height,
					EGL_LINUX_DRM_FOURCC_EXT, img.fourcc,
					EGL_DMA_BUF_PLANE0_FD_EXT, img.fd,
					EGL_DMA_BUF_PLANE0_OFFSET_EXT, img.offset,
					EGL_DMA_BUF_PLANE0_PITCH_EXT, img.pitch,
					EGL_NONE
				};*/
				//eimg_attrs[7] = img.fd;
				eimg = eglCreateImage(edisp, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, 0,
					eimg_attrs);
				ASSERT(eimg);
				
				glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, eimg);
			}
			//MSG("%#x", img.fd);
			
			// rebind texture
			glBindTexture(GL_TEXTURE_2D, tex);
			
			glViewport(0, 0, width, height);
			glClear(GL_COLOR_BUFFER_BIT);

			glUniform2f(glGetUniformLocation(prog, "res"), width, height);
			glRects(-1, -1, 1, 1);

			ASSERT(eglSwapBuffers(edisp, esurf));
		}
	}

exit:
	if (dma_buf_fd >= 0)
		close(dma_buf_fd);
	if (fb)
		drmModeFreeFB(fb);
	eglMakeCurrent(edisp, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(edisp, ectx);
	eglDestroySurface(xdisp, esurf);
	XDestroyWindow(xdisp, xwin);
	free(vinfo);
	eglTerminate(edisp);
	XCloseDisplay(xdisp);
}
