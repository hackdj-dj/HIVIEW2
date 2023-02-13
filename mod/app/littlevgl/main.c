#include "lvgl/lvgl.h"
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/mouse_hid.h"
#include "lv_examples/lv_apps/demo/demo.h"

#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/select.h>

#include "mod/svp/inc/svp.h"
#include "mod/codec/inc/codec.h"

#define DISP_BUF_SIZE (80*LV_HOR_RES_MAX/*1280*/)

static pthread_t tid;
static int lv_running = 0, lv_w = 1280, lv_h = 1024, lt = 1;
static void* lvgl_main(void* p);
static int msq = -1;
static int lv_push_osd = 0;

int lvgl_stop(void)
{
  lv_running = 0;
  return pthread_join(tid, NULL);
}

extern char home_path[128];

int lvgl_start(int w, int h, int push_osd)
{
  lv_w = w;
  lv_h = h;
  lv_push_osd = push_osd;
  lv_running = 1;
  return pthread_create(&tid, NULL, lvgl_main, NULL);
}

int lvgl_ctl(int cmd, void* args)
{
  lt = (int)args;
  return 0;
}


typedef struct msgbuf
{
    long mtype;    /* type of received/sent message */
    char mtext[0]; /* text of the message */
}msgbuf;

static int sub_recv(char *msg, int size, int err)
{
  gsf_msg_t *pmsg = (gsf_msg_t*)msg;
  if(pmsg->id == GSF_EV_SVP_YOLO)
  {
    int len = pmsg->size;
    msgbuf *mbuf = (msgbuf*)(pmsg->data - sizeof(msgbuf));
    mbuf->mtype = 1 + pmsg->ch;
    msgsnd(msq, mbuf, len, IPC_NOWAIT);
  }
  return 0;
}

//send act to GSF_IPC_OSD;
static void fb_flush(lv_disp_drv_t * drv, const lv_area_t * area, lv_color_t * color_p)
{
  static int osd_push = 0;
  static char* osd_buf = NULL;
  static struct fb_var_info var = {0};
  
  fbdev_var(&var);
  
  if(sizeof(lv_color_t) != 2
     || var.xres > 1920 || var.bits_per_pixel != 16)
  {
    //warn("lv_color_t:%d, bits_per_pixel:%d, xres:%d\n", sizeof(lv_color_t), var.xres, var.bits_per_pixel);
    return fbdev_flush(drv, area, color_p);
  }  

  
  if(!osd_push)
  {
    osd_push = nm_push_conn(GSF_IPC_OSD);
    if(!osd_buf)
    {
      osd_buf = malloc(sizeof(gsf_osd_act_t) + DISP_BUF_SIZE*2);
    }
    usleep(100*1000);
    //printf("nm_push_conn osd_push:%d\n", osd_push);
  }
  if(osd_push && osd_buf)
  {
    gsf_osd_act_t *act = (gsf_osd_act_t*)osd_buf;
    
    act->pixel = 16;
    act->x = act->y = 0;
    act->w = var.xres;
    act->h = var.yres;
    act->x1 = area->x1 < 0 ? 0 : area->x1;
    act->y1 = area->y1 < 0 ? 0 : area->y1;
    act->x2 = area->x2 > ((int32_t)var.xres - 1) ? (int32_t)var.xres - 1 : area->x2;
    act->y2 = area->y2 > ((int32_t)var.yres - 1) ? (int32_t)var.yres - 1 : area->y2;
      
    int act_size = (act->x2-act->x1+1)*(act->y2-act->y1+1)*2;
    memcpy(act->data, color_p, act_size);
    nm_push_send_wait(osd_push, (char*)act, sizeof(gsf_osd_act_t)+act_size);
    //printf("nm_push_send_wait act_size:%d\n", act_size);

  }
  return fbdev_flush(drv, area, color_p);
}

static void* lvgl_main(void* p)
{
    /*LittlevGL init*/
    lv_init();

    /*Linux frame buffer device init*/
    fbdev_init();

    /*Initialize and register a display driver*/
    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    
    /*Set the resolution of the display*/
    disp_drv.hor_res = lv_w; //1280
    disp_drv.ver_res = lv_h; //1024
    
    /*A small buffer for LittlevGL to draw the screen's content*/
    static lv_color_t buf[DISP_BUF_SIZE];
    /*Initialize a descriptor for the buffer*/
    static lv_disp_buf_t disp_buf;
    lv_disp_buf_init(&disp_buf, buf, NULL, DISP_BUF_SIZE);
    
    disp_drv.buffer = &disp_buf;
#if defined(GSF_CPU_3516e) || defined(GSF_CPU_x86)
    disp_drv.flush_cb = fbdev_flush;
#else
    disp_drv.flush_cb = lv_push_osd?fb_flush:fbdev_flush;
#endif
    lv_disp_drv_register(&disp_drv);
     
    mouse_hid_init();
    lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);          /*Basic initialization*/
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = mouse_hid_read;     /*This function will be called periodically (by the library) to get the mouse position and state*/
    lv_indev_t * mouse_indev = lv_indev_drv_register(&indev_drv);

    LV_IMG_DECLARE(mouse_ico);
    lv_obj_t * cursor_obj =  lv_img_create(lv_scr_act(), NULL);
    lv_img_set_src(cursor_obj, &mouse_ico);
    //lv_img_set_src(cursor_obj, LV_SYMBOL_CALL);
    lv_indev_set_cursor(mouse_indev, cursor_obj);
    
    lv_coord_t hres = lv_disp_get_hor_res(NULL);
    lv_coord_t vres = lv_disp_get_ver_res(NULL);

    static lv_style_t style_tv_body_bg;
    lv_style_copy(&style_tv_body_bg, &lv_style_plain);
    style_tv_body_bg.body.main_color = lv_color_hex(0x487fb7);
    LV_COLOR_SET_A(style_tv_body_bg.body.main_color, 0x00);
    style_tv_body_bg.body.grad_color = lv_color_hex(0x487fb7);
    LV_COLOR_SET_A(style_tv_body_bg.body.grad_color, 0x00);
    style_tv_body_bg.body.padding.top = 0;
    
    static lv_style_t style_tv_body_fg;
    lv_style_copy(&style_tv_body_fg, &lv_style_plain);
    style_tv_body_fg.body.main_color = LV_COLOR_RED;
    LV_COLOR_SET_A(style_tv_body_fg.body.main_color, 0);
    style_tv_body_fg.body.grad_color = LV_COLOR_RED;
    LV_COLOR_SET_A(style_tv_body_fg.body.grad_color, 0);
    style_tv_body_fg.body.padding.top = 0;
    style_tv_body_fg.body.border.color = LV_COLOR_RED;
    style_tv_body_fg.body.border.width = 4;
    
    static lv_style_t style_label; // lv_style_plain_color
    lv_style_copy(&style_label, &lv_style_plain);
    style_label.text.color = LV_COLOR_WHITE;
    style_label.text.font  = &lv_font_roboto_28;

    static lv_style_t style_label_ol; // lv_style_plain_color
    lv_style_copy(&style_label_ol, &lv_style_plain);
    style_label_ol.text.color = LV_COLOR_WHITE;
    style_label_ol.text.font  = &lv_font_roboto_28;

    #ifdef EXTRA_FREETYPE
    static char font_path[256];
    sprintf(font_path, "%s/cfg/arial.ttf", home_path);

    /*Create a font*/
    static lv_ft_info_t info;
    info.name = font_path;
    info.weight = 48;
    info.style = FT_FONT_STYLE_NORMAL;
    info.mem = NULL;
    if(!lv_ft_font_init(&info)) {
        LV_LOG_ERROR("create failed.");
    }
    style_label.text.color = LV_COLOR_WHITE;
    style_label.text.font  = info.font;
    
    /*Create a font*/
    static lv_ft_info_t info2;
    info2.name = font_path;
    info2.weight = 48;
    info2.style = FT_FONT_STYLE_OUTLINE;
    info2.mem = NULL;
    if(!lv_ft_font_init(&info2)) {
        LV_LOG_ERROR("create failed.");
    }
    style_label_ol.text.color = LV_COLOR_RED;//LV_COLOR_GRAY;
    style_label_ol.text.font  = info2.font;
    #endif

    lv_obj_t *win = lv_obj_create(lv_disp_get_scr_act(NULL), NULL);
    lv_obj_set_size(win, hres, vres);
    lv_obj_set_style(win, &style_tv_body_bg);
    
    #if 1 // HDMI/LCD OUT LOGO;
    lv_obj_t *obj_note = lv_obj_create(win, NULL);
    lv_obj_set_style(obj_note, &style_tv_body_fg);
    lv_obj_set_size(obj_note, hres/1, vres/4/4);
    lv_obj_set_x(obj_note, 0);
    lv_obj_set_y(obj_note, vres/1.5);
    
    #ifdef EXTRA_FREETYPE
    static lv_color_t* cbuf = NULL;
    cbuf = (lv_color_t*)malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR((hres/1-16), (vres/4/4-8)));
    lv_obj_t * canvas = lv_canvas_create(obj_note, NULL);
    lv_canvas_set_buffer(canvas, cbuf, hres/1-16, vres/4/4-8, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(canvas, NULL, LV_ALIGN_IN_LEFT_MID, 8, 0);
    lv_color_t bgcolor = LV_COLOR_RED;//LV_COLOR_SILVER;
    LV_COLOR_SET_A(bgcolor, 0);
    lv_canvas_fill_bg(canvas, bgcolor);
    lv_canvas_draw_text(canvas, 0, 0, hres/1-16, &style_label_ol, "LittlevGL && FreeType && https://github.com/openhisilicon/HIVIEW", LV_LABEL_ALIGN_LEFT);
    lv_canvas_draw_text(canvas, 0, 0, hres/1-16, &style_label, "LittlevGL && FreeType && https://github.com/openhisilicon/HIVIEW", LV_LABEL_ALIGN_LEFT);
    #else
    lv_obj_t *label_note = lv_label_create(obj_note, NULL);
    lv_obj_set_style(label_note, &style_label);
    lv_obj_align(label_note, NULL, LV_ALIGN_IN_LEFT_MID, 8, 0);
    lv_label_set_text(label_note, "LittlevGL && https://github.com/openhisilicon/HIVIEW");
    #endif
    #endif
    
    int i = 0, cnt = 0;
    lv_obj_t *obj[64];
    lv_obj_t *label[64];
    
    for(i = 0; i < 64; i++)
    {
      obj[i] = lv_obj_create(win, NULL);
      label[i] = lv_label_create(obj[i], NULL);
      //lv_obj_set_style(label[i], &lv_style_plain_color);
      lv_obj_set_style(label[i], &style_label);
      lv_obj_set_style(obj[i], &style_tv_body_fg);
      lv_obj_set_size(obj[i], 0, 0);
    }

    /*Create a Demo*/
    #if 0
    lv_obj_t* tv = demo_create();
    #endif
    
    // GSF_PUB_SVP
    msq = msgget(111, IPC_CREAT | 0666);
    void* sub = nm_sub_conn(GSF_PUB_SVP, sub_recv);
    
    /*Handle LitlevGL tasks (tickless mode)*/
    while(lv_running) {
      
        int ret = 0;
        char _buf[16*1024];
        msgbuf *mbuf = (msgbuf*)_buf;
        
        lv_task_handler();
        
        if((ret = msgrcv(msq, _buf, sizeof(_buf), 0, IPC_NOWAIT)) > 0)
        {
          gsf_svp_yolos_t *yolos = (gsf_svp_yolos_t*)mbuf->mtext;
          int chn = mbuf->mtype - 1;
          
          float xr = 0, yr = 0, wr = 0, hr = 0;
          
          if(vres > hres) //for vertical screen
          {
            xr = 0; yr = chn*(vres/2);
            wr = hres; wr/= yolos->w;
            hr = vres/2; hr/= yolos->h;
          }
          else if(lt == 2) //for 2ch
          {
            xr = 0 + chn*(hres/2); yr = vres/4;
            wr = hres/2; wr/= yolos->w;
            hr = vres/2; hr/= yolos->h;
          }
          else  // for 1ch;
          {
            wr = hres; wr/= yolos->w;
            hr = vres; hr/= yolos->h;
          }
          
          yolos->cnt = (yolos->cnt > 64)?64:yolos->cnt;
          
          for(i = yolos->cnt; i < cnt; i++)
          {
            lv_obj_set_size(obj[i], 0, 0);
          }
          
          for(i = 0; i < yolos->cnt; i++)
          {
            lv_label_set_text(label[i], yolos->box[i].label);
            lv_obj_set_size(obj[i], yolos->box[i].rect[2] * wr, yolos->box[i].rect[3] * hr);
            lv_obj_set_x(obj[i], xr + yolos->box[i].rect[0] *wr);
            lv_obj_set_y(obj[i], yr + yolos->box[i].rect[1] *hr);
          }
          cnt = yolos->cnt;
        }
        else
        {
          usleep(5*1000);
        }
    }
    
    return NULL;
}

/*Set in lv_conf.h as `LV_TICK_CUSTOM_SYS_TIME_EXPR`*/
uint32_t custom_tick_get(void)
{
    static uint64_t start_ms = 0;
    if(start_ms == 0) {
        struct timeval tv_start;
        gettimeofday(&tv_start, NULL);
        start_ms = (tv_start.tv_sec * 1000000 + tv_start.tv_usec) / 1000;
    }
    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t now_ms;
    now_ms = (tv_now.tv_sec * 1000000 + tv_now.tv_usec) / 1000;

    uint32_t time_ms = now_ms - start_ms;
    return time_ms;
}
