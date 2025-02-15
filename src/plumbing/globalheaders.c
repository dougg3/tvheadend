/**
 *  Global header modification
 *  Copyright (C) 2010 Andreas Öman
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

#include <assert.h>
#include "tvheadend.h"
#include "streaming.h"
#include "globalheaders.h"

typedef struct globalheaders {
  streaming_target_t gh_input;

  streaming_target_t *gh_output;

  struct th_pktref_queue gh_holdq;

  streaming_start_t *gh_ss;

  int gh_passthru;

} globalheaders_t;

/* note: there up to 2.5 sec diffs in some sources! */
#define MAX_SCAN_TIME   5000  // in ms
#define MAX_NOPKT_TIME  2500  // in ms

/**
 *
 */
static inline int
gh_require_meta(int type)
{
  return type == SCT_HEVC ||
         type == SCT_H264 ||
         type == SCT_MPEG2VIDEO ||
         type == SCT_MP4A ||
         type == SCT_AAC ||
         type == SCT_VORBIS ||
         type == SCT_THEORA;
}

/**
 *
 */
static inline int
gh_is_audiovideo(int type)
{
  return SCT_ISVIDEO(type) || SCT_ISAUDIO(type);
}

/**
 *
 */
static void
gh_flush(globalheaders_t *gh)
{
  if(gh->gh_ss != NULL) {
    streaming_start_unref(gh->gh_ss);
    gh->gh_ss = NULL;
  }

  pktref_clear_queue(&gh->gh_holdq);
}


/**
 *
 */
static void
apply_header(streaming_start_component_t *ssc, th_pkt_t *pkt)
{
  if(ssc->es_frame_duration == 0 && pkt->pkt_duration != 0)
    ssc->es_frame_duration = pkt->pkt_duration;

  if(SCT_ISAUDIO(ssc->es_type) && !ssc->es_channels) {
    ssc->es_channels = pkt->a.pkt_channels;
    ssc->es_sri      = pkt->a.pkt_sri;
    ssc->es_ext_sri  = pkt->a.pkt_ext_sri;
  }

  if(SCT_ISVIDEO(ssc->es_type)) {
    if(pkt->v.pkt_aspect_num && pkt->v.pkt_aspect_den) {
      ssc->es_aspect_num = pkt->v.pkt_aspect_num;
      ssc->es_aspect_den = pkt->v.pkt_aspect_den;
    }
  }

  if(ssc->ssc_gh != NULL)
    return;

  if(pkt->pkt_meta != NULL) {
    ssc->ssc_gh = pkt->pkt_meta;
    pktbuf_ref_inc(ssc->ssc_gh);
    return;
  }

  if (ssc->es_type == SCT_MP4A || ssc->es_type == SCT_AAC) {
    ssc->ssc_gh = pktbuf_alloc(NULL, pkt->a.pkt_ext_sri ? 5 : 2);
    uint8_t *d = pktbuf_ptr(ssc->ssc_gh);

    const int profile = 2; /* AAC LC */
    d[0] = (profile << 3) | ((pkt->a.pkt_sri & 0xe) >> 1);
    d[1] = ((pkt->a.pkt_sri & 0x1) << 7) | (pkt->a.pkt_channels << 3);
    if (pkt->a.pkt_ext_sri) { /* SBR extension */
      d[2] = 0x56;
      d[3] = 0xe5;
      d[4] = 0x80 | ((pkt->a.pkt_ext_sri - 1) << 3);
    }
  }
}


/**
 *
 */
static int
header_complete(streaming_start_component_t *ssc, int not_so_picky)
{
  int is_video = SCT_ISVIDEO(ssc->es_type);
  int is_audio = !is_video && SCT_ISAUDIO(ssc->es_type);

  if((is_audio || is_video) && ssc->es_frame_duration == 0)
    return 0;

  if(is_video) {
    if(!not_so_picky && (ssc->es_aspect_num == 0 || ssc->es_aspect_den == 0 ||
                         ssc->es_width == 0 || ssc->es_height == 0))
      return 0;
  }

  if(is_audio && !ssc->es_channels)
    return 0;

  if(ssc->ssc_gh == NULL && gh_require_meta(ssc->es_type))
    return 0;

  return 1;
}


/**
 *
 */
static int64_t
gh_queue_delay(globalheaders_t *gh, int index)
{
  th_pktref_t *f = TAILQ_FIRST(&gh->gh_holdq);
  th_pktref_t *l = TAILQ_LAST(&gh->gh_holdq, th_pktref_queue);
  streaming_start_component_t *ssc;
  int64_t diff;

  /*
   * Find only packets which require the meta data. Ignore others.
   */
  while (f != l) {
    if (f->pr_pkt->pkt_dts != PTS_UNSET) {
      ssc = streaming_start_component_find_by_index
              (gh->gh_ss, f->pr_pkt->pkt_componentindex);
      if (ssc && ssc->es_index == index)
        break;
    }
    f = TAILQ_NEXT(f, pr_link);
  }
  while (l != f) {
    if (l->pr_pkt->pkt_dts != PTS_UNSET) {
      ssc = streaming_start_component_find_by_index
              (gh->gh_ss, l->pr_pkt->pkt_componentindex);
      if (ssc && ssc->es_index == index)
        break;
    }
    l = TAILQ_PREV(l, th_pktref_queue, pr_link);
  }

  if (l->pr_pkt->pkt_dts != PTS_UNSET && f->pr_pkt->pkt_dts != PTS_UNSET) {
    diff = (l->pr_pkt->pkt_dts & PTS_MASK) - (f->pr_pkt->pkt_dts & PTS_MASK);
    if (diff < 0) {
      diff += PTS_MASK;
      /* hack by Doug, this fixes immediate audio failure due to negative wrap */
      if (diff > PTS_MASK/2)
        diff = 0;
    }
  } else {
    diff = 0;
  }

  /* special noop packet from transcoder, increase decision limit */
  if (l == f && l->pr_pkt->pkt_payload == NULL)
    return 1;

  return diff;
}


/**
 *
 */
static int
headers_complete(globalheaders_t *gh)
{
  streaming_start_t *ss = gh->gh_ss;
  streaming_start_component_t *ssc;
  int64_t *qd = alloca(ss->ss_num_components * sizeof(int64_t));
  int64_t qd_max = 0;
  int i, threshold = 0;

  assert(ss != NULL);

  for(i = 0; i < ss->ss_num_components; i++) {
    ssc = &ss->ss_components[i];
    qd[i] = gh_is_audiovideo(ssc->es_type) ?
              gh_queue_delay(gh, ssc->es_index) : 0;
    if (qd[i] > qd_max)
      qd_max = qd[i];
  }

  if (qd_max <= 0)
    return 0;

  threshold = qd_max > MAX_SCAN_TIME * 90;

  for(i = 0; i < ss->ss_num_components; i++) {
    ssc = &ss->ss_components[i];

    if(!header_complete(ssc, threshold)) {
      /*
       * disable stream only when
       * - half timeout is reached without any packets seen
       * - maximal timeout is reached without metadata
       */
      if(threshold || (qd[i] <= 0 && qd_max > (MAX_NOPKT_TIME * 90))) {
	ssc->ssc_disabled = 1;
        tvhdebug(LS_GLOBALHEADERS, "gh disable stream %d %s%s%s (PID %i) threshold %d qd %"PRId64" qd_max %"PRId64,
             ssc->es_index, streaming_component_type2txt(ssc->es_type),
             ssc->es_lang[0] ? " " : "", ssc->es_lang, ssc->es_pid,
             threshold, qd[i], qd_max);
      } else {
	return 0;
      }
    } else {
      ssc->ssc_disabled = 0;
    }
  }

  if (tvhtrace_enabled()) {
    for(i = 0; i < ss->ss_num_components; i++) {
      ssc = &ss->ss_components[i];
      tvhtrace(LS_GLOBALHEADERS, "stream %d %s%s%s (PID %i) complete time %"PRId64"%s",
               ssc->es_index, streaming_component_type2txt(ssc->es_type),
               ssc->es_lang[0] ? " " : "", ssc->es_lang, ssc->es_pid,
               gh_queue_delay(gh, ssc->es_index),
               ssc->ssc_disabled ? " disabled" : "");
    }
  }

  return 1;
}


/**
 *
 */
static void
gh_start(globalheaders_t *gh, streaming_message_t *sm)
{
  gh->gh_ss = streaming_start_copy(sm->sm_data);
  streaming_msg_free(sm);
}


/**
 *
 */
static void
gh_hold(globalheaders_t *gh, streaming_message_t *sm)
{
  th_pkt_t *pkt;
  streaming_start_component_t *ssc;

  switch(sm->sm_type) {
  case SMT_PACKET:
    pkt = sm->sm_data;
    ssc = streaming_start_component_find_by_index(gh->gh_ss,
						  pkt->pkt_componentindex);
    if (ssc == NULL) {
      tvherror(LS_GLOBALHEADERS, "Unable to find component %d", pkt->pkt_componentindex);
      streaming_msg_free(sm);
      return;
    }

    pkt_trace(LS_GLOBALHEADERS, pkt, "hold receive");

    pkt_ref_inc(pkt);

    if (pkt->pkt_err == 0)
      apply_header(ssc, pkt);

    pktref_enqueue(&gh->gh_holdq, pkt);

    streaming_msg_free(sm);

    if(!gh_is_audiovideo(ssc->es_type))
      break;

    if(!headers_complete(gh))
      break;

    // Send our modified start
    sm = streaming_msg_create_data(SMT_START,
				   streaming_start_copy(gh->gh_ss));
    streaming_target_deliver2(gh->gh_output, sm);

    // Send all pending packets
    while((pkt = pktref_get_first(&gh->gh_holdq)) != NULL) {
      if (pkt->pkt_payload) {
        sm = streaming_msg_create_pkt(pkt);
        streaming_target_deliver2(gh->gh_output, sm);
      }
      pkt_ref_dec(pkt);
    }
    gh->gh_passthru = 1;
    break;

  case SMT_START:
    if (gh->gh_ss)
      gh_flush(gh);
    gh_start(gh, sm);
    break;

  case SMT_STOP:
    gh_flush(gh);
    streaming_msg_free(sm);
    break;

  case SMT_GRACE:
  case SMT_EXIT:
  case SMT_SERVICE_STATUS:
  case SMT_SIGNAL_STATUS:
  case SMT_DESCRAMBLE_INFO:
  case SMT_NOSTART:
  case SMT_NOSTART_WARN:
  case SMT_MPEGTS:
  case SMT_SPEED:
  case SMT_SKIP:
  case SMT_TIMESHIFT_STATUS:
    streaming_target_deliver2(gh->gh_output, sm);
    break;
  }
}


/**
 *
 */
static void
gh_pass(globalheaders_t *gh, streaming_message_t *sm)
{
  th_pkt_t *pkt;
  switch(sm->sm_type) {
  case SMT_START:
    /* stop */
    gh->gh_passthru = 0;
    gh_flush(gh);
    /* restart */
    gh_start(gh, sm);
    break;

  case SMT_STOP:
    gh->gh_passthru = 0;
    gh_flush(gh);
    // FALLTHRU
  case SMT_GRACE:
  case SMT_EXIT:
  case SMT_SERVICE_STATUS:
  case SMT_SIGNAL_STATUS:
  case SMT_DESCRAMBLE_INFO:
  case SMT_NOSTART:
  case SMT_NOSTART_WARN:
  case SMT_MPEGTS:
  case SMT_SKIP:
  case SMT_SPEED:
  case SMT_TIMESHIFT_STATUS:
    streaming_target_deliver2(gh->gh_output, sm);
    break;
  case SMT_PACKET:
    pkt = sm->sm_data;
    if (pkt->pkt_payload || pkt->pkt_err)
      streaming_target_deliver2(gh->gh_output, sm);
    else
      streaming_msg_free(sm);
    break;
  }
}


/**
 *
 */
static void
globalheaders_input(void *opaque, streaming_message_t *sm)
{
  globalheaders_t *gh = opaque;

  if(gh->gh_passthru)
    gh_pass(gh, sm);
  else
    gh_hold(gh, sm);
}

static htsmsg_t *
globalheaders_input_info(void *opaque, htsmsg_t *list)
{
  globalheaders_t *gh = opaque;
  streaming_target_t *st = gh->gh_output;
  htsmsg_add_str(list, NULL, "globalheaders input");
  return st->st_ops.st_info(st->st_opaque, list);
}

static streaming_ops_t globalheaders_input_ops = {
  .st_cb   = globalheaders_input,
  .st_info = globalheaders_input_info
};


/**
 *
 */
streaming_target_t *
globalheaders_create(streaming_target_t *output)
{
  globalheaders_t *gh = calloc(1, sizeof(globalheaders_t));

  TAILQ_INIT(&gh->gh_holdq);

  gh->gh_output = output;
  streaming_target_init(&gh->gh_input, &globalheaders_input_ops, gh, 0);
  return &gh->gh_input;
}


/**
 *
 */
void
globalheaders_destroy(streaming_target_t *pad)
{
  globalheaders_t *gh = (globalheaders_t *)pad;
  gh_flush(gh);
  free(gh);
}
