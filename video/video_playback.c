/*
 *  Playback of video
 *  Copyright (C) 2007-2008 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>


#include <libhts/htssettings.h>

#include <libavformat/avformat.h>
#include <libglw/glw.h>

#include "showtime.h"
#include "layout/layout.h"
#include "video_playback.h"
#include "video_decoder.h"
#include "video_menu.h"
#include "subtitles.h"
#include "event.h"


#define INPUT_APP_SEEK_DIRECT               INPUT_APP + 0

#define OVERLAY_BUTTON_PLAYPAUSE 1
#define OVERLAY_BUTTON_PREV      2
#define OVERLAY_BUTTON_REW       3
#define OVERLAY_BUTTON_FWD       4
#define OVERLAY_BUTTON_END       5
#define OVERLAY_BUTTON_VSETTINGS 6
#define OVERLAY_BUTTON_ASETTINGS 7
#define OVERLAY_BUTTON_SSETTINGS 8
#define OVERLAY_BUTTON_STOP      9

typedef struct play_video_ctrl {
  glw_t *pvc_container;
  glw_t *pvc_status;
  glw_t *pvc_menu;

  appi_t *pvc_ai;

  AVFormatContext *pvc_fctx;

  codecwrap_t **pvc_cwvec;

  vd_conf_t pvc_vdc;
  
  int64_t pvc_rcache_last;

  char *pvc_rcache_title;

  int pvc_force_status_display;

  glw_prop_t *pvc_prop_root;
  glw_prop_t *pvc_prop_playstatus;
  glw_prop_t *pvc_prop_time_current;

  glw_prop_t *pvc_prop_videoinfo;
  glw_prop_t *pvc_prop_audioinfo;

} play_video_ctrl_t;

/**
 * Update text about video/audio stream type
 */
static void
video_player_update_stream_info(play_video_ctrl_t *pvc)
{
  media_pipe_t *mp = pvc->pvc_ai->ai_mp;
  int as = mp->mp_audio.mq_stream;
  int vs = mp->mp_video.mq_stream;
  AVCodecContext *ctx;

  ctx = as >= 0 && pvc->pvc_cwvec[as] != NULL ? 
    pvc->pvc_cwvec[as]->codec_ctx : NULL;
  media_update_codec_info_prop(pvc->pvc_prop_audioinfo, ctx);

  ctx = as >= 0 && pvc->pvc_cwvec[vs] != NULL ? 
    pvc->pvc_cwvec[vs]->codec_ctx : NULL;
  media_update_codec_info_prop(pvc->pvc_prop_videoinfo, ctx);
}

/**
 *
 */
static void
rcache_init(play_video_ctrl_t *pvc, const char *fname)
{
  char *n;
  n = pvc->pvc_rcache_title = strdup(fname);

  while(*n) {
    if(*n == '/' || *n == ':' || *n == '?' || *n == '*' || *n > 127 || *n < 32)
      *n = '_';
    n++;
  }
}

/**
 * Restart cache, store info
 */
static void
rcache_store(play_video_ctrl_t *pvc, int ts)
{
  htsmsg_t *m;

  m = htsmsg_create();
  htsmsg_add_u64(m, "ts", ts);
  hts_settings_save(m, "restartcache/%s", pvc->pvc_rcache_title);
  htsmsg_destroy(m);
}



/**
 * Called from GLW when user selecs a different audio track
 */
static void
video_playback_set_audio_track(void *opaque, void *opaque2, int value)
{
  play_video_ctrl_t *pvc = opaque;

  pvc->pvc_ai->ai_mp->mp_audio.mq_stream = value;
  video_player_update_stream_info(pvc);
}

/**
 * Open menu
 */
static void
video_player_open_menu(play_video_ctrl_t *pvc, int toggle)
{
  glw_t *p;
  AVCodecContext *ctx;
  char buf[100];
  media_pipe_t *mp = pvc->pvc_ai->ai_mp;
  int i;

  if(pvc->pvc_menu != NULL) {
    if(toggle) {
      glw_detach(pvc->pvc_menu);
      pvc->pvc_menu = NULL;
    }
    return;
  }

  pvc->pvc_menu =
    glw_model_create("theme://videoplayer/menu.model", pvc->pvc_container,
		     0, pvc->pvc_prop_root, NULL);
  
  /**
   * Populate audio tracks
   */
  if((p = glw_find_by_id(pvc->pvc_menu, "audio_tracks", 0)) != NULL) {

    for(i = 0; i < pvc->pvc_fctx->nb_streams; i++) {
      ctx = pvc->pvc_fctx->streams[i]->codec;
      if(ctx->codec_type != CODEC_TYPE_AUDIO)
	continue;
      media_get_codec_info(ctx, buf, sizeof(buf));
      glw_selection_add_text_option(p, buf, video_playback_set_audio_track,
				    pvc, NULL, i, i == mp->mp_audio.mq_stream);
    }
    glw_selection_add_text_option(p, "Off", video_playback_set_audio_track,
				  pvc, NULL, -1, -1 == mp->mp_audio.mq_stream);
  }


  /**
   * Populate video control widgets
   */
  video_menu_attach(pvc->pvc_menu, &pvc->pvc_vdc);

}




/**
 *
 */
static void
play_video_clock_update(play_video_ctrl_t *pvc, int64_t pts,
			media_pipe_t *mp)
{
  if(pts != AV_NOPTS_VALUE && pts > pvc->pvc_rcache_last) {

    rcache_store(pvc, pts);

    pvc->pvc_rcache_last = pts + AV_TIME_BASE * 5;
  }

  if(pts != AV_NOPTS_VALUE) {
    pts -= pvc->pvc_fctx->start_time;
    glw_prop_set_time(pvc->pvc_prop_time_current, pts / AV_TIME_BASE);
  }
}



/**
 * Thread for reading from lavf and sending to lavc
 */
static void
video_player_loop(play_video_ctrl_t *pvc, glw_event_queue_t *geq)
{
  appi_t *ai = pvc->pvc_ai;
  media_pipe_t *mp = ai->ai_mp;
  media_buf_t *mb;
  media_queue_t *mq;
  int64_t pts, dts, seek_ref, seek_delta, seek_abs;
  glw_event_t *ge;
  glw_event_appmethod_t *gea;
  event_ts_t *et;
  AVCodecContext *ctx;
  AVPacket pkt;
  int run, i;

  pts          = AV_NOPTS_VALUE;
  dts          = AV_NOPTS_VALUE;
  seek_ref     = pvc->pvc_fctx->start_time;
  run          = 1;

  mp->mp_status_xfader->glw_parent->glw_selected = mp->mp_status_xfader;

  while(run) {
    i = av_read_frame(pvc->pvc_fctx, &pkt);

    if(i < 0) {
      printf("Read error %d\n", i);
      mp_wait(mp, mp->mp_audio.mq_stream != -1, mp->mp_video.mq_stream != -1);
      break;
    }

    ctx = pvc->pvc_cwvec[pkt.stream_index] ? 
      pvc->pvc_cwvec[pkt.stream_index]->codec_ctx : NULL;
    mb = NULL;
    
    /* Rescale PTS / DTS to �sec */
    if(pkt.pts != AV_NOPTS_VALUE) {
      pts = av_rescale_q(pkt.pts, 
			 pvc->pvc_fctx->streams[pkt.stream_index]->time_base,
			 AV_TIME_BASE_Q);
    } else {
      pts = AV_NOPTS_VALUE;
    }
 
    if(pkt.dts != AV_NOPTS_VALUE) {
      dts = av_rescale_q(pkt.dts, 
			 pvc->pvc_fctx->streams[pkt.stream_index]->time_base,
			 AV_TIME_BASE_Q);
    } else {
      dts = AV_NOPTS_VALUE;
    }
 

    if(ctx != NULL) {
      if(pkt.stream_index == mp->mp_video.mq_stream) {
	/* Current video stream */

	mb = media_buf_alloc();
	mb->mb_data_type = MB_VIDEO;
	mq = &mp->mp_video;

      } else if(pkt.stream_index == mp->mp_audio.mq_stream) {
	/* Current audio stream */
	mb = media_buf_alloc();
	mb->mb_data_type = MB_AUDIO;
	mq = &mp->mp_audio;
      }
    }

    if(mb == NULL) {
      av_free_packet(&pkt);
    } else {

      mb->mb_pts = pts;
      mb->mb_dts = dts;
   
      /* Rescale duration */

      mb->mb_duration =
	av_rescale_q(pkt.duration, 
		     pvc->pvc_fctx->streams[pkt.stream_index]->time_base,
		     AV_TIME_BASE_Q);

      
      mb->mb_cw = wrap_codec_ref(pvc->pvc_cwvec[pkt.stream_index]);

      /* Move the data pointers from ffmpeg's packet */

      mb->mb_stream = pkt.stream_index;

      mb->mb_data = pkt.data;
      pkt.data = NULL;

      mb->mb_size = pkt.size;
      pkt.size = 0;

      /* Enqueue packet */

      mb_enqueue(mp, mq, mb);
    }
    av_free_packet(&pkt);

    media_update_playstatus_prop(pvc->pvc_prop_playstatus, mp->mp_playstatus);

    if(mp->mp_playstatus == MP_PLAY && mp_is_audio_silenced(mp))
      mp_set_playstatus(mp, MP_PAUSE);

    ai->ai_req_fullscreen = mp->mp_playstatus == MP_PLAY && !pvc->pvc_menu;

    ge = glw_event_get(mp_is_paused(mp) ? -1 : 0, geq);

    seek_abs   = 0;
    seek_delta = 0;

    if(ge != NULL) {
      switch(ge->ge_type) {

      case GEV_APPMETHOD:
	gea = (void *)ge;

	if(!strcmp(gea->method, "restart")) {
	  seek_abs = 1;
	}

	if(!strcmp(gea->method, "closeMenu")) {
	  
	  if(pvc->pvc_menu != NULL) {
	    glw_detach(pvc->pvc_menu);
	    pvc->pvc_menu = NULL;
	  }
	}

	break;

      case EVENT_VIDEO_CLOCK:
	et = (void *)ge;
	/**
	 * Feedback from decoders
	 */
	if(et->stream != mp->mp_video.mq_stream)
	  break; /* we only deal with the video stream here */

	if(et->dts != AV_NOPTS_VALUE)
	  seek_ref = et->dts;

	play_video_clock_update(pvc, et->pts, mp);
	break;

      case EVENT_KEY_MENU:
	video_player_open_menu(pvc, 1);
	break;

      case EVENT_KEY_STOP:
	run = 0;
	break;
      case EVENT_KEY_RESTART_TRACK:
	seek_abs = 1;
	break;
      case EVENT_KEY_SEEK_FAST_BACKWARD:
	seek_delta = -60000000;
	break;
      case EVENT_KEY_SEEK_BACKWARD:
	seek_delta = -15000000;
	break;
      case EVENT_KEY_SEEK_FAST_FORWARD:
	seek_delta = 60000000;
	break;
      case EVENT_KEY_SEEK_FORWARD:
	seek_delta = 15000000;
	break;

      case EVENT_KEY_PLAYPAUSE:
      case EVENT_KEY_PLAY:
      case EVENT_KEY_PAUSE:
	mp_playpause(mp, ge->ge_type);
	break;
      
      default:
	break;
      }
      glw_event_unref(ge);
    }
    if((seek_delta && seek_ref != AV_NOPTS_VALUE) || seek_abs) {
      /* Seeking requested */

      /* Reset restart cache threshold to force writeout */
      pvc->pvc_rcache_last = INT64_MIN;

      /* Just make it display the seek widget */
      media_update_playstatus_prop(pvc->pvc_prop_playstatus, MP_VIDEOSEEK_PLAY);

      if(seek_abs)
	seek_ref = seek_abs;
      else
	seek_ref += seek_delta;

      if(seek_ref < pvc->pvc_fctx->start_time)
	seek_ref = pvc->pvc_fctx->start_time;

      i = av_seek_frame(pvc->pvc_fctx, -1, seek_ref, AVSEEK_FLAG_BACKWARD);

      if(i < 0)
	printf("Seeking failed\n");

      mp_flush(mp);
      
      mp->mp_videoseekdts = seek_ref;

      switch(mp->mp_playstatus) {
      case MP_VIDEOSEEK_PAUSE:
      case MP_PAUSE:
	mp_set_playstatus(mp, MP_VIDEOSEEK_PAUSE);
	break;
      case MP_VIDEOSEEK_PLAY:
      case MP_PLAY:
	mp_set_playstatus(mp, MP_VIDEOSEEK_PLAY);
	break;
      default:
	abort();
      }
    }
  }
}


/**
 *  Main function for video playback
 */
int
play_video(const char *url, appi_t *ai, glw_event_queue_t *geq, glw_t *parent)
{
  AVCodecContext *ctx;
  formatwrap_t *fw;
  media_pipe_t *mp = ai->ai_mp;
  glw_t *vdw, *top;
  int64_t ts;
  int streams, i;
  play_video_ctrl_t pvc;
  const char *s;
  htsmsg_t *m;
  char faurl[1000];
  glw_prop_t *p;

  memset(&pvc, 0, sizeof(play_video_ctrl_t));
  pvc.pvc_ai = ai;

  /**
   * Open input file
   */

  snprintf(faurl, sizeof(faurl), "showtime:%s", url);
  if(av_open_input_file(&pvc.pvc_fctx, faurl, NULL, 0, NULL) != 0) {
    fprintf(stderr, "Unable to open input file %s\n", url);
    return -1;
  }

  pvc.pvc_fctx->flags |= AVFMT_FLAG_GENPTS;

  if(av_find_stream_info(pvc.pvc_fctx) < 0) {
    av_close_input_file(pvc.pvc_fctx);
    fprintf(stderr, "Unable to find stream info\n");
    return -1;
  }

  /**
   * Create property tree
   */ 

  pvc.pvc_prop_root = glw_prop_create(NULL, "media", GLW_GP_DIRECTORY);

  pvc.pvc_prop_playstatus = glw_prop_create(pvc.pvc_prop_root,
					     "playstatus", GLW_GP_STRING);


  p = glw_prop_create(pvc.pvc_prop_root, "time", GLW_GP_DIRECTORY);
  glw_prop_set_time(glw_prop_create(p, "total", GLW_GP_TIME),
		    pvc.pvc_fctx->duration / AV_TIME_BASE);

  pvc.pvc_prop_time_current = glw_prop_create(p, "current", GLW_GP_TIME);

  pvc.pvc_prop_videoinfo = glw_prop_create(pvc.pvc_prop_root,
					    "videoinfo", GLW_GP_STRING);

  pvc.pvc_prop_audioinfo = glw_prop_create(pvc.pvc_prop_root,
					    "audioinfo", GLW_GP_STRING);

  s = pvc.pvc_fctx->title;
  if(*s == 0) {
    /* No stored title */
    s = strrchr(url, '/');
    s = s ? s + 1 : url;
  }
  
  glw_prop_set_string(glw_prop_create(pvc.pvc_prop_root, 
				      "title", GLW_GP_STRING), s);

  /**
   * Create top level widget
   */
  top = glw_model_create("theme://videoplayer/videoplayer.model", parent,
			 0, pvc.pvc_prop_root, NULL);
  pvc.pvc_container = glw_find_by_id(top, "videoplayer_container", 0);
  if(pvc.pvc_container == NULL) {
    fprintf(stderr, "Unable to locate videoplayer container\n");
    sleep(1);
    glw_destroy(top);
    av_close_input_file(pvc.pvc_fctx);
    glw_prop_destroy(pvc.pvc_prop_root);
    return -1;
  }

  /**
   * Create video output widget
   */
  vd_conf_init(&pvc.pvc_vdc);
  vdw = vd_create_widget(pvc.pvc_container, ai->ai_mp, 1.0);
  mp_set_video_conf(mp, &pvc.pvc_vdc);


  /**
   * Status overlay
   */
  pvc.pvc_status = glw_model_create("theme://videoplayer/status.model",
				    mp->mp_status_xfader,
				    0, pvc.pvc_prop_root, NULL);

  /**
   * Init codec contexts
   */
  pvc.pvc_cwvec = alloca(pvc.pvc_fctx->nb_streams * sizeof(void *));
  memset(pvc.pvc_cwvec, 0, sizeof(void *) * pvc.pvc_fctx->nb_streams);
  
  mp->mp_audio.mq_stream = -1;
  mp->mp_video.mq_stream = -1;

  fw = wrap_format_create(pvc.pvc_fctx, 1);

  for(i = 0; i < pvc.pvc_fctx->nb_streams; i++) {
    ctx = pvc.pvc_fctx->streams[i]->codec;

    if(mp->mp_video.mq_stream == -1 && ctx->codec_type == CODEC_TYPE_VIDEO) {
      mp->mp_video.mq_stream = i;
    }

    if(mp->mp_audio.mq_stream == -1 && ctx->codec_type == CODEC_TYPE_AUDIO) {
      mp->mp_audio.mq_stream = i;
    }

    pvc.pvc_cwvec[i] = wrap_codec_create(ctx->codec_id,
					 ctx->codec_type, 0, fw, ctx);
  }

  ai->ai_fctx   = pvc.pvc_fctx;
  mp->mp_format = pvc.pvc_fctx;

  wrap_lock_all_codecs(fw);
  
  /**
   * Restart playback at last position
   */

  mp->mp_videoseekdts = 0;

  rcache_init(&pvc, url);

  m = hts_settings_load("restartcache/%s", pvc.pvc_rcache_title);
  if(m != NULL) {
    if(!htsmsg_get_s64(m, "ts", &ts)) {
      i = av_seek_frame(pvc.pvc_fctx, -1, ts, AVSEEK_FLAG_BACKWARD);
      if(i >= 0)
	mp->mp_videoseekdts = ts;
    }
  }

  mp_set_playstatus(mp, MP_VIDEOSEEK_PAUSE); 

  mp->mp_feedback = geq;

  video_player_update_stream_info(&pvc);

  wrap_unlock_all_codecs(fw);


  pvc.pvc_rcache_last = INT64_MIN;


  video_player_loop(&pvc, geq);


  glw_destroy(pvc.pvc_status);

  ai->ai_req_fullscreen = 0;

  mp_set_playstatus(mp, MP_STOP);

  wrap_lock_all_codecs(fw);

  mp->mp_total_time = 0;

  ai->ai_fctx = NULL;

  streams = pvc.pvc_fctx->nb_streams;

  for(i = 0; i < streams; i++)
    if(pvc.pvc_cwvec[i] != NULL)
      wrap_codec_deref(pvc.pvc_cwvec[i], 0);

  glw_destroy(top);

  wrap_format_wait(fw);

  if(mp->mp_subtitles) {
    subtitles_free(mp->mp_subtitles);
    mp->mp_subtitles = NULL;
  }
  glw_event_flushqueue(geq);
  return 0;
}
