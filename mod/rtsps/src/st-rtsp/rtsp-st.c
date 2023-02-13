﻿#include "rtsps.h"
#include "rtsp-st.h"

#include "rtsp-server-st.h"
#include "rtsp-client-st.h"

#include "h26xbits.h"
//#include "fw/h26xbits/src/h264_stream.h"

#include "inc/frm.h"

#define AFRAME_MAX_SIZE (2*1024)

static pthread_t rtsp_st_pid;
static void* st_rtsp_ctl_listen(char *ip, unsigned short port);

struct rtsp_conn_ctx_t
{
  UT_hash_handle hh;
  char name[256];
  int  refcnt;
  void* c;
  int video_shmid;
  int audio_shmid;
  struct cfifo_ex *video_fifo;
  struct cfifo_ex *audio_fifo;
  gsf_frm_t *video_frm; // gsf_frm_t + __data_size__;
  int packet_cnt;
  int last_time;
};

struct rtsp_push_ctx_t
{
  UT_hash_handle hh;
  char name[256];
  int  refcnt;
  void* c;
  int err;
  int last_time;
};


// muti-rtsps, conn_ctxs in shm memery;
static struct rtsp_conn_ctx_t *conn_ctxs;
static struct rtsp_push_ctx_t *push_ctxs;


static unsigned int cfifo_recrel(unsigned char *p1, unsigned int n1, unsigned char *p2)
{
    return 0;
}

static unsigned int cfifo_recput(unsigned char *p1, unsigned int n1, unsigned char *p2, void *u)
{
  int i = 0, a = 0, l = 0, _n1 = n1;
  unsigned char *p = NULL, *_p1 = p1, *_p2 = p2;
  gsf_frm_t *rec = (gsf_frm_t*)u;

  p = (unsigned char*)(rec);
  a = sizeof(gsf_frm_t) + rec->size;
  
  l = CFIFO_MIN(a, _n1);
  memcpy(_p1, p, l);
  memcpy(_p2, p+l, a-l);
  _n1-=l;_p1+=l;_p2+=a-l;

  return 0;
}

static int onframe(void* param, const char*encoding, const void *packet, int bytes, uint32_t time, int flags)
{
  const uint8_t start_code[] = { 0, 0, 0, 1 };
  struct rtsp_conn_ctx_t *ctx = (struct rtsp_conn_ctx_t *)param; 
  rtsp_client_keepalive(ctx->c, 0);
  
  if(flags)
  {
    printf("%s => encoding:%s, time:%08u, flags:%08d, drop.\n", ctx->name, encoding, time, flags);
  }

	struct timespec _ts;  
  clock_gettime(CLOCK_MONOTONIC, &_ts);
  ctx->last_time = _ts.tv_sec;

	if (0 == strcmp("H264", encoding))
	{
		uint8_t type = *(uint8_t*)packet & 0x1f;

		if(!ctx->video_frm)
		{
		  ctx->video_frm = (gsf_frm_t*)malloc(sizeof(gsf_frm_t)+GSF_FRM_MAX_SIZE);
		  memset(ctx->video_frm, 0, sizeof(gsf_frm_t));
		  ctx->packet_cnt = 0;
		}
		
    if (type == 7) // sps;
		{
      h26x_parse_sps_wh((char*)packet
          , type
          , bytes
          , &ctx->video_frm->video.width
          , &ctx->video_frm->video.height);
		}
	#if 0 //"fw/h26xbits/src/h264_stream.h"
    if(0)//if (type == 7)
    {
      h264_stream_t* h = h264_new();
      read_nal_unit(h, packet, bytes);
     
      h->sps->vui.bitstream_restriction_flag = 0;
      h->sps->vui.timing_info_present_flag = 0;
      printf("rebuild nal_type:%d, vui.bitstream_restriction_flag:%d\n", type, h->sps->vui.bitstream_restriction_flag);
      write_nal_unit(h, packet, bytes);
      h264_free(h);
    }
	#endif
	
    if(ctx->video_frm->size + 4 + bytes <= GSF_FRM_MAX_SIZE)
    {
      ctx->video_frm->data[ctx->video_frm->size+0] = 00;
      ctx->video_frm->data[ctx->video_frm->size+1] = 00;
      ctx->video_frm->data[ctx->video_frm->size+2] = 00;
      ctx->video_frm->data[ctx->video_frm->size+3] = 01;
      
	    memcpy(ctx->video_frm->data + ctx->video_frm->size + 4, packet, bytes);
	    ctx->video_frm->video.nal[ctx->packet_cnt] = bytes + 4;
	    ctx->video_frm->size += bytes + 4;
	    ctx->packet_cnt++;
	  }
	  else
	  {
	    ctx->video_frm->size = 0;
	    ctx->packet_cnt = 0;
      return 0;
	  }

		if(type == 5 || type == 1)
		{
      ctx->video_frm->type = GSF_FRM_VIDEO;
      ctx->video_frm->flag = (type == 5)?GSF_FRM_FLAG_IDR:0;
      ctx->video_frm->seq  = ctx->video_frm->seq + 1;
      ctx->video_frm->utc  = _ts.tv_sec*1000 + _ts.tv_nsec/1000000;
      ctx->video_frm->pts  = ctx->video_frm->utc;
      ctx->video_frm->video.encode = GSF_ENC_H264;

      int ret = cfifo_put(ctx->video_fifo,  
                      ctx->video_frm->size+sizeof(gsf_frm_t),
                      cfifo_recput, 
                      (unsigned char*)ctx->video_frm);
      if(0)                
      printf("%s => encoding:%s, time:%08u, type:%d, flags:%08d, w:%d, h:%d, packet_cnt:%d\n"
			      , ctx->name, encoding, time, type, flags
			      , ctx->video_frm->video.width, ctx->video_frm->video.height
			      , ctx->packet_cnt);
			      
      ctx->video_frm->size = 0;
	    ctx->packet_cnt = 0;
		}
	}
	else if (0 == strcmp("H265", encoding))
	{
		uint8_t type = (*(uint8_t*)packet >> 1) & 0x3f;
		if (type > 32) // !vcl;
		{
			printf("%s => encoding:%s, time:%08u, flags:%08d, bytes:%d\n", ctx->name, encoding, time, flags, bytes);
		}
	}
  else if (0 == strcmp("mpeg4-generic", encoding))
  {
    char audio_buf[AFRAME_MAX_SIZE];
    gsf_frm_t *audio_frm = (gsf_frm_t *)audio_buf;
    if(bytes + 7 + sizeof(gsf_frm_t) <= sizeof(audio_buf))
    {
      uint8_t ADTS[] = {0xFF, 0xF1, 0x00, 0x00, 0x00, 0x00, 0xFC}; 
      int audioSamprate = 32000;//音频采样
      int audioChannel = 2;     //音频声道
      int audioBit = 16;        //固定
      switch(audioSamprate)
      {
        case  16000: ADTS[2] = 0x60; break;
        case  32000: ADTS[2] = 0x54; break;
        case  44100: ADTS[2] = 0x50; break;
        case  48000: ADTS[2] = 0x4C; break;
        case  96000: ADTS[2] = 0x40; break;
        default: break;
      }
      ADTS[3] = (audioChannel==2)?0x80:0x40;

      int len = bytes + 7;
      len <<= 5;//8bit * 2 - 11 = 5(headerSize 11bit)
      len |= 0x1F;//5 bit    1            
      ADTS[4] = len>>8;
      ADTS[5] = len & 0xFF;
      memcpy(audio_frm->data, ADTS, sizeof(ADTS));
      
      audio_frm->type = GSF_FRM_AUDIO;
      audio_frm->flag = GSF_FRM_FLAG_IDR;
      audio_frm->seq  = 0;
      audio_frm->utc  = 0;
      audio_frm->pts  = audio_frm->utc;
      audio_frm->size = bytes + 7;
      audio_frm->audio.encode = GSF_ENC_AAC;
      
      memcpy(audio_frm->data + 7, packet, bytes);
      int ret = cfifo_put(ctx->audio_fifo,
                      audio_frm->size+sizeof(gsf_frm_t),
                      cfifo_recput, 
                      (unsigned char*)audio_frm);
      if(0)
      printf("%s=>encoding:%s,time:%08u,bytes:%d,packet[%02X %02X %02X %02X]\n"
        , ctx->name, encoding, time, bytes
        , *((uint8_t*)packet+0), *((uint8_t*)packet+1), *((uint8_t*)packet+2), *((uint8_t*)packet+3));
    }
  }  
  return 0;
}


static void* rtsp_st_thread(void *param)
{
  int *run_flag = (int*)param;
  
  st_set_eventsys(ST_EVENTSYS_ALT);
  /* Initialize the ST library */
  if (st_init() < 0) {
    printf("st_init err.\n");
    *run_flag = -1;  
    return NULL;
  }
  
#ifdef GSF_CPU_x86
  void *l = st_rtsp_server_listen(NULL, 8554);
#else
  void *l = st_rtsp_server_listen(NULL, 554);
  void *c = st_rtsp_ctl_listen("127.0.0.1", 127);
#endif

  *run_flag = 1;
  while(1)
  {
    st_sleep(1000);
  }

  return NULL; 
}


int rtsp_st_init()
{
  //lo;
  system("ifconfig lo 127.0.0.1");
  
  //set keepalive parm
	system("echo 60 > /proc/sys/net/ipv4/tcp_keepalive_time");
	system("echo 10 > /proc/sys/net/ipv4/tcp_keepalive_intvl");
	system("echo 5 > /proc/sys/net/ipv4/tcp_keepalive_probes");

	//set SO_SNDBUF 800k;
	system("echo  819200 > /proc/sys/net/core/wmem_max");
	system("echo  819200 > /proc/sys/net/core/wmem_default");
	system("echo  819200 > /proc/sys/net/core/rmem_max");
	system("echo  819200 > /proc/sys/net/core/rmem_default");
  
  int run_flag = 0;
  
  int ret = pthread_create(&rtsp_st_pid, NULL, rtsp_st_thread, &run_flag);
  if(ret < 0)
    return ret;
  
  while(!run_flag)
    usleep(10*1000);
    
  return ret;
}

int rtsp_st_ctl(struct rtsp_st_ctl_t *ctl)
{
  int ret = 0;
  struct sockaddr_in to_addr;
  memset(&to_addr, 0, sizeof(struct sockaddr_in));
  to_addr.sin_family = AF_INET;
  to_addr.sin_port = htons(127);
  to_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  
  int sock = socket(PF_INET, SOCK_DGRAM, 0);
  
  struct timeval tv;
	tv.tv_sec  = 3;
	tv.tv_usec = 0;
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  
  ret = sendto(sock, ctl, sizeof(*ctl)+ctl->size, 0, (struct sockaddr*)&to_addr, sizeof(to_addr));
  printf("sendto ret:%d\n", ret);
  
  if(ret > 0)
  {
    struct sockaddr_in from_addr;
    int from_len = sizeof(struct sockaddr_in);

    ret = recvfrom(sock, ctl, 8*1024, 0, (struct sockaddr*)&from_addr, &from_len);
    printf("recvfrom ret:%d\n", ret);
    close(sock);
    return (ret>0)?0:-1;
  }
  close(sock);
  return -1;
}

static int push_err(void *param, int err)
{
  struct rtsp_push_ctx_t *ctx = (struct rtsp_push_ctx_t *)param;
  ctx->err = err;
  printf("ctx:%p, err:%d\n", ctx, err);
  return 0;
}

static void push_ctx_check(void)
{
  struct timespec _ts;  
  clock_gettime(CLOCK_MONOTONIC, &_ts);
  
  struct rtsp_push_ctx_t *ctx, *tmp;
  HASH_ITER(hh, push_ctxs, ctx, tmp)
  {
    if(_ts.tv_sec - ctx->last_time > 5)
    {
      if(ctx->err == 0)
      {
        ctx->last_time = _ts.tv_sec;
        continue;
      }
      
      //clear error;
      ctx->err = 0;  
      printf("reconnect => ctx->name:[%s], cur:%d, last:%d\n"
            , ctx->name, _ts.tv_sec, ctx->last_time);
      
      if(ctx->c)
      {
        rtsp_client_close(ctx->c);
      }
      
      struct st_rtsp_client_handler_t handler;
      handler.onframe = NULL;
      handler.onerr   = push_err;
      handler.param   = (void*)ctx;
      ctx->c = rtsp_client_connect(ctx->name, RTSP_TRANSPORT_RTP_UDP, &handler);         
      
      ctx->last_time = _ts.tv_sec;
    }
  }
}


static void conn_ctx_check(void)
{
  struct timespec _ts;  
  clock_gettime(CLOCK_MONOTONIC, &_ts);
  
  struct rtsp_conn_ctx_t *ctx, *tmp;
  HASH_ITER(hh, conn_ctxs, ctx, tmp)
  {
    if(_ts.tv_sec - ctx->last_time > 5)
    {
      printf("reconnect => ctx->name:[%s], cur:%d, last:%d\n"
            , ctx->name, _ts.tv_sec, ctx->last_time);
      
      if(ctx->c)
      {
        rtsp_client_close(ctx->c);
      }
      
      struct st_rtsp_client_handler_t handler;
      handler.onframe = onframe;
      handler.onerr   = NULL;
      handler.param   = (void*)ctx;
      ctx->c = rtsp_client_connect(ctx->name, RTSP_TRANSPORT_RTP_UDP, &handler);         
      
      ctx->last_time = _ts.tv_sec;
    }
  }
}

static void conn_ctx_open(struct rtsp_st_ctl_t *ctl)
{
  gsf_rtsp_url_t *ru = (gsf_rtsp_url_t*)ctl->data;
  
  char *name = ru->url;
  if(strlen(name) == 0 || strlen(name) > 255)
  {
    printf("id: GSF_ID_RTSPS_C_OPEN err name [%s]\n", name);
    ctl->size = 0;
    return;
  }
  
  struct rtsp_conn_ctx_t *ctx = NULL;
  HASH_FIND_STR(conn_ctxs, name, ctx);
  if (ctx)
  {
    ctx->refcnt++;
    printf("id: GSF_ID_RTSPS_C_OPEN refcnt:%d [%s]\n", ctx->refcnt, name);
    
    gsf_shmid_t *shmid = (gsf_shmid_t*)ctl->data;
    shmid->video_shmid = ctx->video_shmid;
    shmid->audio_shmid = ctx->audio_shmid;;
    ctl->size = sizeof(gsf_shmid_t);
    ctl->err = 0;
    return;
  }
  
  ctx = calloc(1, sizeof(*ctx));
  ctx->refcnt++;
  ctx->audio_shmid = -1;
  ctx->video_fifo = cfifo_alloc(2*GSF_FRM_MAX_SIZE,
                  cfifo_recsize, 
                  cfifo_rectag, 
                  cfifo_recrel, 
                  &ctx->video_shmid,
                  0);
  ctx->audio_fifo = cfifo_alloc(16*AFRAME_MAX_SIZE,
                  cfifo_recsize, 
                  cfifo_rectag, 
                  cfifo_recrel, 
                  &ctx->audio_shmid,
                  0);
                  
  strncpy(ctx->name, name, sizeof(ctx->name)-1);          
  printf("id: GSF_ID_RTSPS_C_OPEN [%s] video_shmid:%d, audio_shmid:%d\n"
        , name, ctx->video_shmid, ctx->audio_shmid);
  
  struct timespec _ts;  
  clock_gettime(CLOCK_MONOTONIC, &_ts);
  ctx->last_time = _ts.tv_sec;
  
  struct st_rtsp_client_handler_t handler;
  handler.onframe = onframe;
  handler.onerr   = NULL;
  handler.param   = (void*)ctx;
  ctx->c = rtsp_client_connect(name, RTSP_TRANSPORT_RTP_UDP, &handler);
  HASH_ADD_STR(conn_ctxs, name, ctx);
  
  gsf_shmid_t *shmid = (gsf_shmid_t*)ctl->data;
  shmid->video_shmid = ctx->video_shmid;
  shmid->audio_shmid = ctx->audio_shmid;
  ctl->size = sizeof(gsf_shmid_t);
  ctl->err = 0;
  return;
}

static void conn_ctx_close(struct rtsp_st_ctl_t *ctl)
{
  gsf_rtsp_url_t *ru = (gsf_rtsp_url_t*)ctl->data;
  
  char *name = ru->url;
  struct rtsp_conn_ctx_t *ctx = NULL;
  if(strlen(name) == 0 || strlen(name) > 255)
  {
    ctl->size = 0;
    return;
  }
  HASH_FIND_STR(conn_ctxs, name, ctx);
  if (ctx && --ctx->refcnt == 0)
  {
    HASH_DEL(conn_ctxs, ctx);
    
    printf("id: GSF_ID_RTSPS_C_CLOSE free [%s]\n", name);
    if(ctx->c)
    {
      rtsp_client_close(ctx->c);
    }
    if(ctx->video_shmid != -1)
    {
      cfifo_free(ctx->video_fifo);
    }
    if(ctx->audio_shmid != -1)
    {
      cfifo_free(ctx->audio_fifo);
    }
    free(ctx);
  }
  else if(ctx)
  {
    printf("id: GSF_ID_RTSPS_C_CLOSE refcnt:%d, [%s]\n", ctx->refcnt, name);
  }
  
  ctl->size = 0;
  ctl->err = 0;
  return;
}


static void conn_ctx_list(struct rtsp_st_ctl_t *ctl)
{
  struct rtsp_conn_ctx_t *ctx, *tmp;
  HASH_ITER(hh, conn_ctxs, ctx, tmp)
  {
    printf("GSF_ID_RTSPS_C_LIST ctx->name:[%s]\n", ctx->name);
  }
  ctl->size = 0;
  ctl->err = 0;
}
static void conn_ctx_clear(struct rtsp_st_ctl_t *ctl)
{
  struct rtsp_conn_ctx_t *ctx, *tmp;
  HASH_ITER(hh, conn_ctxs, ctx, tmp)
  {
    HASH_DEL(conn_ctxs, ctx);

    printf("GSF_ID_RTSPS_C_CLEAR ctx->name:[%s]\n", ctx->name);
    
    if(ctx->c)
    {
      rtsp_client_close(ctx->c);
    }
    if(ctx->video_shmid != -1)
    {
      cfifo_free(ctx->video_fifo);
    }
    if(ctx->audio_shmid != -1)
    {
      cfifo_free(ctx->audio_fifo);
    }
    free(ctx);
  }
  ctl->size = 0;
  ctl->err = 0;
}



static void push_ctx_open(struct rtsp_st_ctl_t *ctl)
{
  gsf_rtsp_purl_t *pu = (gsf_rtsp_purl_t*)ctl->data;
  
  char *name = pu->url;
  
  if(strlen(name) == 0 || strlen(name) > 255)
  {
    printf("id: GSF_ID_RTSPS_P_OPEN err name [%s]\n", name);
    ctl->size = 0;
    return;
  }
  
  struct rtsp_push_ctx_t *ctx = NULL;
  HASH_FIND_STR(push_ctxs, name, ctx);
  if (ctx)
  {
    ctx->refcnt++;
    printf("id: GSF_ID_RTSPS_P_OPEN refcnt:%d [%s]\n", ctx->refcnt, name);
    ctl->size = 0;
    ctl->err = 0;
    return;
  }
  
  ctx = calloc(1, sizeof(*ctx));
  ctx->refcnt++;
  strncpy(ctx->name, name, sizeof(ctx->name)-1);          
  printf("id: GSF_ID_RTSPS_P_OPEN [%s] \n", name);
  
  struct timespec _ts;  
  clock_gettime(CLOCK_MONOTONIC, &_ts);
  ctx->last_time = _ts.tv_sec;
  
  struct st_rtsp_client_handler_t handler;
  handler.ch = pu->ch;
  handler.st = pu->st;
  handler.onframe = NULL; // pusher;
  handler.onerr   = push_err;
  handler.param   = (void*)ctx;
  ctx->c = rtsp_client_connect(name, RTSP_TRANSPORT_RTP_UDP, &handler);
  HASH_ADD_STR(push_ctxs, name, ctx);
  
  ctl->size = 0;
  ctl->err = 0;
  return;
}


static void push_ctx_close(struct rtsp_st_ctl_t *ctl)
{
  gsf_rtsp_purl_t *pu = (gsf_rtsp_purl_t*)ctl->data;
  
  char *name = pu->url;
  if(strlen(name) == 0 || strlen(name) > 255)
  {
    ctl->size = 0;
    return;
  }
  
  struct rtsp_push_ctx_t *ctx = NULL;
  HASH_FIND_STR(push_ctxs, name, ctx);
  if (ctx && --ctx->refcnt == 0)
  {
    HASH_DEL(push_ctxs, ctx);
    
    printf("id: GSF_ID_RTSPS_P_CLOSE free [%s]\n", name);
    if(ctx->c)
    {
      rtsp_client_close(ctx->c);
    }
    free(ctx);
  }
  else if(ctx)
  {
    printf("id: GSF_ID_RTSPS_P_CLOSE refcnt:%d, [%s]\n", ctx->refcnt, name);
  }
  
  ctl->size = 0;
  ctl->err  = 0;
}

static void push_ctx_list(struct rtsp_st_ctl_t *ctl)
{
  printf("id: GSF_ID_RTSPS_P_LIST ...\n");
  ctl->size = 0;
  ctl->err = 0;
}
static void push_ctx_clear(struct rtsp_st_ctl_t *ctl)
{
  printf("id: GSF_ID_RTSPS_P_CLEAR ...\n");
  ctl->size = 0;
  ctl->err = 0;
}


static void *handle_ctl(void *arg)
{
  char buffer[8*1024];
  st_netfd_t srv_nfd = (st_netfd_t)arg;
  
  while(1)
  {
    struct sockaddr_in from_addr;
    int from_len = sizeof(struct sockaddr_in);
    memset(buffer, 0, sizeof(buffer));
    int ret = st_recvfrom(srv_nfd, buffer, sizeof(buffer)
              , (struct sockaddr*)&from_addr, &from_len
              , 3*1000*1000);
    
    if (ret <= 0)
    {
      conn_ctx_check();
      push_ctx_check();
      continue;
    }

    struct rtsp_st_ctl_t *ctl = (struct rtsp_st_ctl_t*)buffer;
    printf("recv ctl->id:%d, size:%d\n", ctl->id, ctl->size);
    
    ctl->err = -1; // default error
    switch(ctl->id)
    {
      case GSF_ID_RTSPS_C_OPEN:  { conn_ctx_open(ctl);  break; }
      case GSF_ID_RTSPS_C_CLOSE: { conn_ctx_close(ctl); break; }
      case GSF_ID_RTSPS_C_LIST:  { conn_ctx_list(ctl);  break; }
      case GSF_ID_RTSPS_C_CLEAR: { conn_ctx_clear(ctl); break; }
      case GSF_ID_RTSPS_P_OPEN:  { push_ctx_open(ctl);  break; }
      case GSF_ID_RTSPS_P_CLOSE: { push_ctx_close(ctl); break; }
      case GSF_ID_RTSPS_P_LIST:  { push_ctx_list(ctl);  break; }
      case GSF_ID_RTSPS_P_CLEAR: { push_ctx_clear(ctl); break; }
      default:
        ctl->size = 0;
        break;
    }
    printf("send ctl->id:%d, size:%d\n", ctl->id, ctl->size);
    st_sendto(srv_nfd, ctl, sizeof(*ctl)+ctl->size
            , (struct sockaddr*)&from_addr, from_len
            , ST_UTIME_NO_TIMEOUT);
  }
  st_netfd_close(srv_nfd);
}



static void* st_rtsp_ctl_listen(char *ip, unsigned short port)
{
  int sock, n;
  struct sockaddr_in lcl_addr;
  st_netfd_t srv_nfd;
  memset(&lcl_addr, 0, sizeof(struct sockaddr_in));
  lcl_addr.sin_family = AF_INET;
  lcl_addr.sin_port = htons(port);
  lcl_addr.sin_addr.s_addr = inet_addr(ip);

  if ((sock = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
    printf("socket err.\n");
    return NULL;
  }
  n = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&n, sizeof(n)) < 0) {
    printf("setsockopt SO_REUSEADDR err.\n");
    return NULL;
  }

  if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (char *)&n, sizeof(n))<0) {
    printf("setsockopt SO_REUSEPORT err.\n");
    //return NULL;
  }
  
  if (bind(sock, (struct sockaddr *)&lcl_addr, sizeof(lcl_addr)) < 0) {
    printf("bind err.");
    return NULL;
  }
  if ((srv_nfd = st_netfd_open_socket(sock)) == NULL) {
    printf("st_netfd_open err.");
    return NULL;
  }
  st_thread_t tid;
  if ((tid = st_thread_create(handle_ctl, srv_nfd, 0, 0)) == NULL) {
    st_netfd_close(srv_nfd);
    printf("st_thread_create err.\n");
    return NULL;
  }
  printf("ok tid:%p\n", tid);
  
  return (void*)tid;
}
