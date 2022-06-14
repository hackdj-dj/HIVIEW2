#if defined(GSF_CPU_3516d) || defined(GSF_CPU_3559)

#include "mod/rtsps/inc/rtsps.h"
#include "codec.h"
#include "cfg.h"
#include "msg_func.h"
#include "mpp.h"

#define PT_VENC(t) \
            (t == GSF_ENC_H264)? PT_H264:\
            (t == GSF_ENC_H265)? PT_H265:\
            (t == GSF_ENC_JPEG)? PT_JPEG:\
            (t == GSF_ENC_MJPEG)? PT_MJPEG:\
            PT_H264  
//cfifo;
extern unsigned int cfifo_recsize(unsigned char *p1, unsigned int n1, unsigned char *p2);
extern unsigned int cfifo_rectag(unsigned char *p1, unsigned int n1, unsigned char *p2);
extern unsigned int cfifo_recrel(unsigned char *p1, unsigned int n1, unsigned char *p2);
static unsigned int cfifo_recgut(unsigned char *p1, unsigned int n1, unsigned char *p2, void *u)
{
  unsigned int len = cfifo_recsize(p1, n1, p2);
  unsigned int l = CFIFO_MIN(len, n1);
  char *p = (char*)u;
  memcpy(p, p1, l);
  memcpy(p+l, p2, len-l);
  return len;
}  
  
int vdec_connect(struct cfifo_ex** fifo, gsf_frm_t** frm)
{
  gsf_shmid_t shmid = {-1, -1};
  
  GSF_MSG_DEF(gsf_rtsp_url_t, rtsp_url, 8*1024); 
  rtsp_url->transp = 0;
  
  strcpy(rtsp_url->url, codec_ipc.vdec.rtsp);
  int ret = GSF_MSG_SENDTO(GSF_ID_RTSPS_C_OPEN, 0, SET, 0
                        , sizeof(gsf_rtsp_url_t)
                        , GSF_IPC_RTSPS, 3000);
  if(ret == 0 && __pmsg->err == 0)
  {
    shmid = *((gsf_shmid_t*)__pmsg->data);
    printf("video_shmid:%d\n", shmid.video_shmid);
  }

  if(shmid.video_shmid >= 0)
  {
    fifo[0] = cfifo_shmat(cfifo_recsize, cfifo_rectag, shmid.video_shmid);
    frm[0] = (gsf_frm_t*)malloc(1000*1024);
    cfifo_newest(fifo[0], 1);
    cfifo_set_u(fifo[0], (void*)1);
    printf("video fifo:%p, frm:%p\n", fifo[0], frm[0]);
  }
  if(shmid.audio_shmid >= 0)
  {
    fifo[1] = cfifo_shmat(cfifo_recsize, cfifo_rectag, shmid.audio_shmid);
    frm[1] = (gsf_frm_t*)malloc(4*1024);
    cfifo_newest(fifo[1], 1);
    cfifo_set_u(fifo[1], (void*)1);
    printf("audio fifo:%p, frm:%p\n", fifo[1], frm[1]);
  }
  
  return 0;
}

int vo_sendfrm(struct cfifo_ex** fifo, gsf_frm_t** frm)
{
  int ret = 0, i = 0;
  if(!fifo[0] || !frm[0])
    return -1;
  
  for(i = 0; i < 2; i++)
  while(fifo[i] && frm[i])
  {
    int ret = cfifo_get(fifo[i], cfifo_recgut, (void*)frm[i]);
    if(ret <= 0)
    {
      //printf("err: cfifo_get i:%d, ret:%d\n", i, ret);
      break;
    }
    int ch = (int)(cfifo_get_u(fifo[i]));
    char *data = frm[i]->data;
    
    gsf_mpp_frm_attr_t attr;
    attr.size   = frm[i]->size;     // data size;
    attr.ftype  = frm[i]->flag;     // frame type;
    attr.pts    = frm[i]->pts*1000; // pts ms*1000;
    
    if(frm[i]->type == GSF_FRM_VIDEO)
    {  
      attr.etype  = PT_VENC(frm[i]->video.encode);// PAYLOAD_TYPE_E;
      attr.width  = frm[i]->video.width;   // width;
      attr.height = frm[i]->video.height;  // height;
      ret = gsf_mpp_vo_vsend(VOLAYER_HD0, ch, data, &attr);
      //printf("video size:%d, pts:%llu\n", attr.size, attr.pts);
    }
    else if(frm[i]->type == GSF_FRM_AUDIO)
    {
      //printf("audio size:%d, pts:%llu\n", attr.size, attr.pts);
      attr.etype  = PT_AAC;
      ret = gsf_mpp_ao_asend(SAMPLE_AUDIO_INNER_HDMI_AO_DEV, ch, data, &attr);
    }
  };
  usleep(10*1000);
  
  
  return 0;
}

static int _task_runing = 1;

void* vdec_task(void *param)
{
  struct cfifo_ex* fifo[2] = {NULL,};
  gsf_frm_t *frm[2] = {NULL,};
  
  vdec_connect(fifo, frm);
  
  while(_task_runing)
  {
    if(vo_sendfrm(fifo, frm) < 0)
    {
      usleep(10*1000);
    }
  }
  return NULL;
}


static pthread_t pid;

int vdec_start()
{
  if(!codec_ipc.vdec.en)
    return -1;
 
  if(!strlen(codec_ipc.vdec.rtsp)) //"rtsp://admin:ydjs123456@192.168.1.25:554"
    return -1;   
  
  int ly = VO_LAYOUT_2MUX;      
  gsf_mpp_vo_layout(VOLAYER_HD0, ly, NULL);
  
  extern int vo_ly_set(int ly);
  vo_ly_set(ly);
  
  gsf_mpp_vo_src_t src0 = {0, 0};
  gsf_mpp_vo_bind(VOLAYER_HD0, 0, &src0);

  return pthread_create(&pid, 0, vdec_task, NULL);
}

#else
int vdec_start()
{
  return -1;
}
#endif // defined(GSF_CPU_3516d) || defined(GSF_CPU_3559)