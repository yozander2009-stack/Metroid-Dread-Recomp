/* Builds without the front end (Linux, or SWR_NO_FRONTEND / no runtime/imgui/libimgui.a): the hooks the backend and
   the GL queue call do nothing, so the game starts directly and F1 does nothing. */
int fe_should_show(int argc, char** argv, const char* exedir, const char* exename){ (void)argc; (void)argv; (void)exedir; (void)exename; return 0; }
int fe_run(void* hwnd){ (void)hwnd; return 0; }
int fe_wndproc(void* h, unsigned m, unsigned long long w, long long l, long long* r){ (void)h; (void)m; (void)w; (void)l; (void)r; return 0; }
int ov_wndproc(void* h, unsigned m, unsigned long long w, long long l){ (void)h; (void)m; (void)w; (void)l; return 0; }
int ov_is_open(void){ return 0; }
int ov_pad(unsigned long long phys, int lx, int ly){ (void)phys; (void)lx; (void)ly; return 0; }
void ov_gl_frame(void){}
/* no menu here: a second Esc within 2 seconds quits (#38) */
void backend_request_quit(void); extern void (*g_backend_status)(const char*); unsigned long long plat_ns(void);
void ov_escape(void){ static unsigned long long last; unsigned long long now=plat_ns(); if(last && now-last<2000000000ull){ backend_request_quit(); return; } last=now; if(g_backend_status) g_backend_status("Press Esc again to quit"); }
