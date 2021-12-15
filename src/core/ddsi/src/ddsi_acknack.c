/*
 * Copyright(c) 2020 ADLINK Technology Limited and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */

#include "dds/ddsrt/static_assert.h"
#include "dds/ddsi/q_rtps.h"
#include "dds/ddsi/q_radmin.h"
#include "dds/ddsi/q_misc.h"
#include "dds/ddsi/q_bswap.h"
#include "dds/ddsi/q_xmsg.h"
#include "dds/ddsi/q_log.h"
#include "dds/ddsi/q_bitset.h"
#include "dds/ddsi/ddsi_domaingv.h"
#include "dds/ddsi/ddsi_acknack.h"
#include "dds/ddsi/ddsi_entity_index.h"
#include "dds/ddsi/ddsi_security_omg.h"

#define ACK_REASON_IN_FLAGS 0

static seqno_t next_deliv_seq (const struct proxy_writer *pwr, const seqno_t next_seq)
{
  /* We want to determine next_deliv_seq, the next sequence number to
     be delivered to all in-sync readers, so that we can acknowledge
     what we have actually delivered.  This is different from next_seq
     tracks, which tracks the sequence number up to which all samples
     have been received.  The difference is the delivery queue.

     There is always but a single delivery queue, and hence delivery
     thread, associated with a single proxy writer; but the ACKs are
     always generated by another thread.  Therefore, updates to
     next_deliv_seq need only be atomic with respect to these reads.
     On all supported platforms we can atomically load and store 32
     bits without issue, and so we store just the low word of the
     sequence number.

     We know 1 <= next_deliv_seq AND next_seq - N <= next_deliv_seq <=
     next_seq for N << 2**32.  With n = next_seq, nd = next_deliv_seq,
     H the upper half and L the lower half:

       - H(nd) <= H(n) <= H(nd)+1         { n >= nd AND N << 2*32}
       - H(n) = H(nd)   => L(n) >= L(nd)  { n >= nd }
       - H(n) = H(nd)+1 => L(n) < L(nd)   { N << 2*32 }

     Therefore:

       L(n) < L(nd) <=> H(n) = H(nd+1)

     a.k.a.:

       nd = nd' - if nd' > nd then 2**32 else 0
         where nd' = 2**32 * H(n) + L(nd)

     By not locking next_deliv_seq, we may have nd a bit lower than it
     could be, but that only means we are acknowledging slightly less
     than we could; but that is perfectly acceptible.

     FIXME: next_seq - #dqueue could probably be used instead,
     provided #dqueue is decremented after delivery, rather than
     before delivery. */
  const uint32_t lw = ddsrt_atomic_ld32 (&pwr->next_deliv_seq_lowword);
  seqno_t next_deliv_seq;
  next_deliv_seq = (next_seq & ~(seqno_t) UINT32_MAX) | lw;
  if (next_deliv_seq > next_seq)
    next_deliv_seq -= ((seqno_t) 1) << 32;
  assert (0 < next_deliv_seq && next_deliv_seq <= next_seq);
  return next_deliv_seq;
}

static void add_AckNack_getsource (const struct proxy_writer *pwr, const struct pwr_rd_match *rwn, struct nn_reorder **reorder, seqno_t *bitmap_base, int *notail)
{
  /* if in sync, look at proxy writer status, else look at proxy-writer--reader match status */
  if (rwn->in_sync == PRMSS_OUT_OF_SYNC || rwn->filtered)
  {
    *reorder = rwn->u.not_in_sync.reorder;
    *bitmap_base = nn_reorder_next_seq (*reorder);
    *notail = 0;
  }
  else
  {
    *reorder = pwr->reorder;
    if (!pwr->e.gv->config.late_ack_mode)
    {
      *bitmap_base = nn_reorder_next_seq (*reorder);
      *notail = 0;
    }
    else
    {
      *bitmap_base = next_deliv_seq (pwr, nn_reorder_next_seq (*reorder));
      *notail = nn_dqueue_is_full (pwr->dqueue);
    }
  }
}

DDSRT_STATIC_ASSERT ((NN_SEQUENCE_NUMBER_SET_MAX_BITS % 32) == 0 && (NN_FRAGMENT_NUMBER_SET_MAX_BITS % 32) == 0);
struct add_AckNack_info {
  bool nack_sent_on_nackdelay;
#if ACK_REASON_IN_FLAGS
  uint8_t flags;
#endif
  struct {
    struct nn_sequence_number_set_header set;
    uint32_t bits[NN_FRAGMENT_NUMBER_SET_MAX_BITS / 32];
  } acknack;
  struct {
    seqno_t seq;
    struct nn_fragment_number_set_header set;
    uint32_t bits[NN_FRAGMENT_NUMBER_SET_MAX_BITS / 32];
  } nackfrag;
};

static bool add_AckNack_makebitmaps (const struct proxy_writer *pwr, const struct pwr_rd_match *rwn, struct add_AckNack_info *info)
{
  struct nn_reorder *reorder;
  seqno_t bitmap_base;
  int notail; /* notail = false: all known missing ones are nack'd */
  add_AckNack_getsource (pwr, rwn, &reorder, &bitmap_base, &notail);

  /* Make bitmap; note that we've made sure to have room for the maximum bitmap size. */
  const seqno_t last_seq = rwn->filtered ? rwn->last_seq : pwr->last_seq;
  const uint32_t numbits = nn_reorder_nackmap (reorder, bitmap_base, last_seq, &info->acknack.set, info->acknack.bits, NN_SEQUENCE_NUMBER_SET_MAX_BITS, notail);
  if (numbits == 0)
  {
    info->nackfrag.seq = 0;
    return false;
  }

  /* Scan through bitmap, cutting it off at the first missing sample that the defragmenter
     knows about. Then note the sequence number & add a NACKFRAG for that sample */
  info->nackfrag.seq = 0;
  const seqno_t base = fromSN (info->acknack.set.bitmap_base);
  for (uint32_t i = 0; i < numbits; i++)
  {
    if (!nn_bitset_isset (numbits, info->acknack.bits, i))
      continue;

    const seqno_t seq = base + i;
    const uint32_t fragnum = (seq == pwr->last_seq) ? pwr->last_fragnum : UINT32_MAX;
    switch (nn_defrag_nackmap (pwr->defrag, seq, fragnum, &info->nackfrag.set, info->nackfrag.bits, NN_FRAGMENT_NUMBER_SET_MAX_BITS))
    {
      case DEFRAG_NACKMAP_UNKNOWN_SAMPLE:
        break;
      case DEFRAG_NACKMAP_ALL_ADVERTISED_FRAGMENTS_KNOWN:
        /* Cut the NACK short (or make it an ACK if this is the first sample), no NACKFRAG */
        info->nackfrag.seq = 0;
        info->acknack.set.numbits = i;
        return (i > 0);
      case DEFRAG_NACKMAP_FRAGMENTS_MISSING:
        /* Cut the NACK short, NACKFRAG */
        info->nackfrag.seq = seq;
        info->acknack.set.numbits = i;
        return true;
    }
  }
  return true;
}

static void add_NackFrag (struct nn_xmsg *msg, const struct proxy_writer *pwr, const struct pwr_rd_match *rwn, const struct add_AckNack_info *info)
{
  struct nn_xmsg_marker sm_marker;
  NackFrag_t *nf;

  assert (info->nackfrag.set.numbits > 0 && info->nackfrag.set.numbits <= NN_FRAGMENT_NUMBER_SET_MAX_BITS);
  nf = nn_xmsg_append (msg, &sm_marker, NACKFRAG_SIZE (info->nackfrag.set.numbits));

  nn_xmsg_submsg_init (msg, sm_marker, SMID_NACK_FRAG);
  nf->readerId = nn_hton_entityid (rwn->rd_guid.entityid);
  nf->writerId = nn_hton_entityid (pwr->e.guid.entityid);
  nf->writerSN = toSN (info->nackfrag.seq);
#if ACK_REASON_IN_FLAGS
  nf->smhdr.flags |= info->flags;
#endif
  // We use 0-based fragment numbers, but externally have to provide 1-based fragment numbers */
  nf->fragmentNumberState.bitmap_base = info->nackfrag.set.bitmap_base + 1;
  nf->fragmentNumberState.numbits = info->nackfrag.set.numbits;
  memcpy (nf->bits, info->nackfrag.bits, NN_FRAGMENT_NUMBER_SET_BITS_SIZE (info->nackfrag.set.numbits));

  // Count field is at a variable offset ... silly DDSI spec
  nn_count_t * const countp =
    (nn_count_t *) ((char *) nf + offsetof (NackFrag_t, bits) + NN_FRAGMENT_NUMBER_SET_BITS_SIZE (nf->fragmentNumberState.numbits));
  *countp = pwr->nackfragcount;

  nn_xmsg_submsg_setnext (msg, sm_marker);

  if (pwr->e.gv->logconfig.c.mask & DDS_LC_TRACE)
  {
    ETRACE (pwr, "nackfrag #%"PRIu32":%"PRId64"/%"PRIu32"/%"PRIu32":",
            pwr->nackfragcount, fromSN (nf->writerSN),
            nf->fragmentNumberState.bitmap_base, nf->fragmentNumberState.numbits);
    for (uint32_t ui = 0; ui != nf->fragmentNumberState.numbits; ui++)
      ETRACE (pwr, "%c", nn_bitset_isset (nf->fragmentNumberState.numbits, nf->bits, ui) ? '1' : '0');
  }

  // Encode the sub-message when needed
  encode_datareader_submsg (msg, sm_marker, pwr, &rwn->rd_guid);
}

static void add_AckNack (struct nn_xmsg *msg, const struct proxy_writer *pwr, const struct pwr_rd_match *rwn, const struct add_AckNack_info *info)
{
  /* If pwr->have_seen_heartbeat == 0, no heartbeat has been received
     by this proxy writer yet, so we'll be sending a pre-emptive
     AckNack.  NACKing data now will most likely cause another NACK
     upon reception of the first heartbeat, and so cause the data to
     be resent twice. */
  AckNack_t *an;
  struct nn_xmsg_marker sm_marker;

  an = nn_xmsg_append (msg, &sm_marker, ACKNACK_SIZE_MAX);
  nn_xmsg_submsg_init (msg, sm_marker, SMID_ACKNACK);
  an->readerId = nn_hton_entityid (rwn->rd_guid.entityid);
  an->writerId = nn_hton_entityid (pwr->e.guid.entityid);

  // set FINAL flag late, in case it is decided that the "response_required" flag
  // should be set depending on the exact AckNack/NackFrag generated
  an->smhdr.flags |= ACKNACK_FLAG_FINAL;
#if ACK_REASON_IN_FLAGS
  an->smhdr.flags |= info->flags;
#endif
  an->readerSNState = info->acknack.set;
  memcpy (an->bits, info->acknack.bits, NN_SEQUENCE_NUMBER_SET_BITS_SIZE (an->readerSNState.numbits));

  // Count field is at a variable offset ... silly DDSI spec
  nn_count_t * const countp =
    (nn_count_t *) ((char *) an + offsetof (AckNack_t, bits) + NN_SEQUENCE_NUMBER_SET_BITS_SIZE (an->readerSNState.numbits));
  *countp = rwn->count;
  // Reset submessage size, now that we know the real size, and update the offset to the next submessage.
  nn_xmsg_shrink (msg, sm_marker, ACKNACK_SIZE (an->readerSNState.numbits));
  nn_xmsg_submsg_setnext (msg, sm_marker);

  if (pwr->e.gv->logconfig.c.mask & DDS_LC_TRACE)
  {
    ETRACE (pwr, "acknack "PGUIDFMT" -> "PGUIDFMT": F#%"PRIu32":%"PRId64"/%"PRIu32":",
            PGUID (rwn->rd_guid), PGUID (pwr->e.guid), rwn->count,
            fromSN (an->readerSNState.bitmap_base), an->readerSNState.numbits);
    for (uint32_t ui = 0; ui != an->readerSNState.numbits; ui++)
      ETRACE (pwr, "%c", nn_bitset_isset (an->readerSNState.numbits, an->bits, ui) ? '1' : '0');
  }

  // Encode the sub-message when needed
  encode_datareader_submsg (msg, sm_marker, pwr, &rwn->rd_guid);
}

static enum add_AckNack_result get_AckNack_info (const struct proxy_writer *pwr, const struct pwr_rd_match *rwn, struct last_nack_summary *nack_summary, struct add_AckNack_info *info, bool ackdelay_passed, bool nackdelay_passed)
{
  /* If pwr->have_seen_heartbeat == 0, no heartbeat has been received
     by this proxy writer yet, so we'll be sending a pre-emptive
     AckNack.  NACKing data now will most likely cause another NACK
     upon reception of the first heartbeat, and so cause the data to
     be resent twice. */
  enum add_AckNack_result result;

#if ACK_REASON_IN_FLAGS
  info->flags = 0;
#endif
  if (!add_AckNack_makebitmaps (pwr, rwn, info))
  {
    info->nack_sent_on_nackdelay = rwn->nack_sent_on_nackdelay;
    nack_summary->seq_base = fromSN (info->acknack.set.bitmap_base);
    nack_summary->seq_end_p1 = 0;
    nack_summary->frag_base = 0;
    nack_summary->frag_end_p1 = 0;
    result = AANR_ACK;
  }
  else
  {
    // [seq_base:0 .. seq_end_p1:0) + [seq_end_p1:frag_base .. seq_end_p1:frag_end_p1) if frag_end_p1 > 0
    const seqno_t seq_base = fromSN (info->acknack.set.bitmap_base);
    assert (seq_base >= 1 && (info->acknack.set.numbits > 0 || info->nackfrag.seq > 0));
    assert (info->nackfrag.seq == 0 || info->nackfrag.set.numbits > 0);
    const seqno_t seq_end_p1 = seq_base + info->acknack.set.numbits;
    const uint32_t frag_base = (info->nackfrag.seq > 0) ? info->nackfrag.set.bitmap_base : 0;
    const uint32_t frag_end_p1 = (info->nackfrag.seq > 0) ? info->nackfrag.set.bitmap_base + info->nackfrag.set.numbits : 0;

    /* Let caller know whether it is a nack, and, in steady state, set
       final to prevent a response if it isn't.  The initial
       (pre-emptive) acknack is different: it'd be nice to get a
       heartbeat in response.

       Who cares about an answer to an acknowledgment!? -- actually,
       that'd a very useful feature in combination with directed
       heartbeats, or somesuch, to get reliability guarantees. */
    nack_summary->seq_end_p1 = seq_end_p1;
    nack_summary->frag_end_p1 = frag_end_p1;
    nack_summary->seq_base = seq_base;
    nack_summary->frag_base = frag_base;

    // [seq_base:0 .. seq_end_p1:0) and [seq_end_p1:frag_base .. seq_end_p1:frag_end_p1) if frag_end_p1 > 0
    if (seq_base > rwn->last_nack.seq_end_p1 || (seq_base == rwn->last_nack.seq_end_p1 && frag_base >= rwn->last_nack.frag_end_p1))
    {
      // A NACK for something not previously NACK'd or NackDelay passed, update nack_{seq,frag} to reflect
      // the changed state
      info->nack_sent_on_nackdelay = false;
#if ACK_REASON_IN_FLAGS
      info->flags = 0x10;
#endif
      result = AANR_NACK;
    }
    else if (rwn->directed_heartbeat && (!rwn->nack_sent_on_nackdelay || nackdelay_passed))
    {
      info->nack_sent_on_nackdelay = false;
#if ACK_REASON_IN_FLAGS
      info->flags = 0x20;
#endif
      result = AANR_NACK;
    }
    else if (nackdelay_passed)
    {
      info->nack_sent_on_nackdelay = true;
#if ACK_REASON_IN_FLAGS
      info->flags = 0x30;
#endif
      result = AANR_NACK;
    }
    else
    {
      // Overlap between this NACK and the previous one and NackDelay has not yet passed: clear numbits and
      // nackfrag_numbits to turn the NACK into an ACK and pretend to the caller nothing scary is going on.
#if ACK_REASON_IN_FLAGS
      info->flags = 0x40;
#endif
      info->nack_sent_on_nackdelay = rwn->nack_sent_on_nackdelay;
      info->acknack.set.numbits = 0;
      info->nackfrag.seq = 0;
      result = AANR_SUPPRESSED_NACK;
    }
  }

  if (result == AANR_ACK || result == AANR_SUPPRESSED_NACK)
  {
    // ACK and SUPPRESSED_NACK both end up being a pure ACK; send those only if we have to
    if (!(rwn->heartbeat_since_ack && rwn->ack_requested))
      result = AANR_SUPPRESSED_ACK; // writer didn't ask for it
    else if (!(nack_summary->seq_base > rwn->last_nack.seq_base || ackdelay_passed))
      result = AANR_SUPPRESSED_ACK; // no progress since last, not enough time passed
  }
  else if (info->acknack.set.numbits == 0 && info->nackfrag.seq > 0 && !rwn->ack_requested)
  {
    // if we are not NACK'ing full samples and we are NACK'ing fragments, skip the ACKNACK submessage if we
    // have no interest in a HEARTBEAT and the writer hasn't asked for an ACKNACK since the last one we sent.
    result = AANR_NACKFRAG_ONLY;
  }
  return result;
}

void sched_acknack_if_needed (struct xevent *ev, struct proxy_writer *pwr, struct pwr_rd_match *rwn, ddsrt_mtime_t tnow, bool avoid_suppressed_nack)
{
  // This is the relatively expensive and precise code to determine what the ACKNACK event will do,
  // the alternative is to do:
  //
  //   add_AckNack_getsource (pwr, rwn, &reorder, &bitmap_base, &notail);
  //   const seqno_t last_seq = rwn->filtered ? rwn->last_seq : pwr->last_seq;
  //   if (bitmap_base <= last_seq)
  //     (void) resched_xevent_if_earlier (ev, tnow);
  //   else if (!(rwn->heartbeat_since_ack && rwn->ack_requested))
  //     ; // writer didn't ask for it
  //   else if (!(bitmap_base > rwn->last_nack.seq_base || ackdelay_passed))
  //     ; // no progress since last, not enough time passed
  //   else
  //    (void) resched_xevent_if_earlier (ev, tnow);
  //
  // which is a stripped-down version of the same logic that more aggressively schedules the event,
  // relying on the event handler to suppress unnecessary messages.  There doesn't seem to be a big
  // downside to being precise.

  struct ddsi_domaingv * const gv = pwr->e.gv;
  const bool ackdelay_passed = (tnow.v >= ddsrt_mtime_add_duration (rwn->t_last_ack, gv->config.ack_delay).v);
  const bool nackdelay_passed = (tnow.v >= ddsrt_mtime_add_duration (rwn->t_last_nack, gv->config.nack_delay).v);
  struct add_AckNack_info info;
  struct last_nack_summary nack_summary;
  const enum add_AckNack_result aanr =
    get_AckNack_info (pwr, rwn, &nack_summary, &info, ackdelay_passed, nackdelay_passed);
  if (aanr == AANR_SUPPRESSED_ACK)
    ; // nothing to be done now
  else if (avoid_suppressed_nack && aanr == AANR_SUPPRESSED_NACK)
    (void) resched_xevent_if_earlier (ev, ddsrt_mtime_add_duration (rwn->t_last_nack, gv->config.nack_delay));
  else
    (void) resched_xevent_if_earlier (ev, tnow);
}

struct nn_xmsg *make_and_resched_acknack (struct xevent *ev, struct proxy_writer *pwr, struct pwr_rd_match *rwn, ddsrt_mtime_t tnow, bool avoid_suppressed_nack)
{
  struct ddsi_domaingv * const gv = pwr->e.gv;
  struct nn_xmsg *msg;
  struct add_AckNack_info info;

  struct last_nack_summary nack_summary;
  const enum add_AckNack_result aanr =
    get_AckNack_info (pwr, rwn, &nack_summary, &info,
                      tnow.v >= ddsrt_mtime_add_duration (rwn->t_last_ack, gv->config.ack_delay).v,
                      tnow.v >= ddsrt_mtime_add_duration (rwn->t_last_nack, gv->config.nack_delay).v);

  if (aanr == AANR_SUPPRESSED_ACK)
    return NULL;
  else if (avoid_suppressed_nack && aanr == AANR_SUPPRESSED_NACK)
  {
    (void) resched_xevent_if_earlier (ev, ddsrt_mtime_add_duration (rwn->t_last_nack, gv->config.nack_delay));
    return NULL;
  }

  // Committing to sending a message in response: update the state.  Note that there's still a
  // possibility of not sending a message, but that is only in case of failures of some sort.
  // Resetting the flags and bailing out simply means we will wait until the next heartbeat to
  // do try again.
  rwn->directed_heartbeat = 0;
  rwn->heartbeat_since_ack = 0;
  rwn->heartbeatfrag_since_ack = 0;
  rwn->nack_sent_on_nackdelay = (info.nack_sent_on_nackdelay ? 1 : 0);

  struct participant *pp = NULL;
  if (q_omg_proxy_participant_is_secure (pwr->c.proxypp))
  {
    struct reader *rd = entidx_lookup_reader_guid (pwr->e.gv->entity_index, &rwn->rd_guid);
    if (rd)
      pp = rd->c.pp;
  }

  if ((msg = nn_xmsg_new (gv->xmsgpool, &rwn->rd_guid, pp, ACKNACK_SIZE_MAX, NN_XMSG_KIND_CONTROL)) == NULL)
  {
    return NULL;
  }

  nn_xmsg_setdstPWR (msg, pwr);
  if (gv->config.meas_hb_to_ack_latency && rwn->hb_timestamp.v)
  {
    // If HB->ACK latency measurement is enabled, and we have a
    // timestamp available, add it and clear the time stamp.  There
    // is no real guarantee that the two match, but I haven't got a
    // solution for that yet ...  If adding the time stamp fails,
    // too bad, but no reason to get worried. */
    nn_xmsg_add_timestamp (msg, rwn->hb_timestamp);
    rwn->hb_timestamp.v = 0;
  }

  if (aanr != AANR_NACKFRAG_ONLY)
    add_AckNack (msg, pwr, rwn, &info);
  if (info.nackfrag.seq > 0)
  {
    ETRACE (pwr, " + ");
    add_NackFrag (msg, pwr, rwn, &info);
  }
  ETRACE (pwr, "\n");
  if (nn_xmsg_size (msg) == 0)
  {
    // attempt at encoding the message caused it to be dropped
    nn_xmsg_free (msg);
    return NULL;
  }

  rwn->count++;
  switch (aanr)
  {
    case AANR_SUPPRESSED_ACK:
      // no message: caught by the size = 0 check
      assert (0);
      break;
    case AANR_ACK:
      rwn->ack_requested = 0;
      rwn->t_last_ack = tnow;
      rwn->last_nack.seq_base = nack_summary.seq_base;
      break;
    case AANR_NACK:
    case AANR_NACKFRAG_ONLY:
      if (nack_summary.frag_end_p1 != 0)
        pwr->nackfragcount++;
      if (aanr != AANR_NACKFRAG_ONLY)
      {
        rwn->ack_requested = 0;
        rwn->t_last_ack = tnow;
      }
      rwn->last_nack = nack_summary;
      rwn->t_last_nack = tnow;
      /* If NACKing, make sure we don't give up too soon: even though
       we're not allowed to send an ACKNACK unless in response to a
       HEARTBEAT, I've seen too many cases of not sending an NACK
       because the writing side got confused ...  Better to recover
       eventually. */
      (void) resched_xevent_if_earlier (ev, ddsrt_mtime_add_duration (tnow, gv->config.auto_resched_nack_delay));
      break;
    case AANR_SUPPRESSED_NACK:
      rwn->ack_requested = 0;
      rwn->t_last_ack = tnow;
      rwn->last_nack.seq_base = nack_summary.seq_base;
      (void) resched_xevent_if_earlier (ev, ddsrt_mtime_add_duration (rwn->t_last_nack, gv->config.nack_delay));
      break;
  }
  GVTRACE ("send acknack(rd "PGUIDFMT" -> pwr "PGUIDFMT")\n", PGUID (rwn->rd_guid), PGUID (pwr->e.guid));
  return msg;
}
