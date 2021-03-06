/*
 * Copyright (c) 2010, Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Contiki operating system.
 *
 */

/**
 * \file
 *         An implementation of RPL's objective function 0, RFC6552
 *
 * \author Joakim Eriksson <joakime@sics.se>, Nicolas Tsiftes <nvt@sics.se>
 */

/**
 * \addtogroup uip6
 * @{
 */

#include "net/rpl/rpl.h"
#include "net/rpl/rpl-private.h"
#include "net/nbr-table.h"
#include "net/link-stats.h"

#define DEBUG DEBUG_NONE
#include "net/ip/uip-debug.h"

static void reset(rpl_dag_t *);
	static rpl_parent_t *best_parent(rpl_parent_t *, rpl_parent_t *);
	static rpl_dag_t *best_dag(rpl_dag_t *, rpl_dag_t *);
	static rpl_rank_t calculate_rank(rpl_parent_t *, rpl_rank_t);
	static void update_metric_container(rpl_instance_t *);
	
	
	rpl_of_t rpl_of0 = {
	  reset,
	  NULL,
	  best_parent,
	  best_dag,
	  calculate_rank,
	  update_metric_container,
	  0
	};
	
	#define DEFAULT_RANK_INCREMENT  RPL_MIN_HOPRANKINC
	
	
	
	#define MIN_DIFFERENCE (RPL_MIN_HOPRANKINC + RPL_MIN_HOPRANKINC / 2)

/* Constants from RFC6552. We use the default values. */
#define RANK_STRETCH       0 /* Must be in the range [0;5] */
#define RANK_FACTOR        1 /* Must be in the range [1;4] */

#define MIN_STEP_OF_RANK   1
#define MAX_STEP_OF_RANK   9

/* OF0 computes rank increase as follows:
 * rank_increase = (RANK_FACTOR * STEP_OF_RANK + RANK_STRETCH) * min_hop_rank_increase
 * STEP_OF_RANK is an implementation-specific scalar value in the range [1;9].
 * RFC6552 provides a default value of 3 but recommends to use a dynamic link metric
 * such as ETX.
 * */

#define RPL_OF0_FIXED_SR      0
#define RPL_OF0_ETX_BASED_SR  1
/* Select RPL_OF0_FIXED_SR or RPL_OF0_ETX_BASED_SR */
#ifdef RPL_OF0_CONF_SR
#define RPL_OF0_SR            RPL_OF0_CONF_SR
#else /* RPL_OF0_CONF_SR */
#define RPL_OF0_SR            RPL_OF0_ETX_BASED_SR
#endif /* RPL_OF0_CONF_SR */

#if RPL_OF0_FIXED_SR
#define STEP_OF_RANK(p)       (3)
#endif /* RPL_OF0_FIXED_SR */

#if RPL_OF0_ETX_BASED_SR
/* Numbers suggested by P. Thubert for in the 6TiSCH WG. Anything that maps ETX to
 * a step between 1 and 9 works. */
#define STEP_OF_RANK(p)       (((3 * parent_link_metric(p)) / LINK_STATS_ETX_DIVISOR) - 2)
#endif /* RPL_OF0_ETX_BASED_SR */

/*---------------------------------------------------------------------------*/
static void
reset(rpl_dag_t *dag)
{
  PRINTF("RPL: Reset OF0\n");
}
/*---------------------------------------------------------------------------*/
#if RPL_WITH_DAO_ACK
static void
dao_ack_callback(rpl_parent_t *p, int status)
{
  if(status == RPL_DAO_ACK_UNABLE_TO_ADD_ROUTE_AT_ROOT) {
    return;
  }
  /* here we need to handle failed DAO's and other stuff */
  PRINTF("RPL: OF0 - DAO ACK received with status: %d\n", status);
  if(status >= RPL_DAO_ACK_UNABLE_TO_ACCEPT) {
    /* punish the ETX as if this was 10 packets lost */
    link_stats_packet_sent(rpl_get_parent_lladdr(p), MAC_TX_OK, 10);
  } else if(status == RPL_DAO_ACK_TIMEOUT) { /* timeout = no ack */
    /* punish the total lack of ACK with a similar punishment */
    link_stats_packet_sent(rpl_get_parent_lladdr(p), MAC_TX_OK, 10);
  }
}
#endif /* RPL_WITH_DAO_ACK */
/*---------------------------------------------------------------------------*/
static uint16_t
parent_link_metric(rpl_parent_t *p)
{
  /* OF0 operates without metric container; the only metric we have is ETX */
  const struct link_stats *stats = rpl_get_parent_link_stats(p);
  return stats != NULL ? stats->etx : 0xffff;
}
/*---------------------------------------------------------------------------*/
static uint16_t
parent_rank_increase(rpl_parent_t *p)
{
  uint16_t min_hoprankinc;
  if(p == NULL || p->dag == NULL || p->dag->instance == NULL) {
    return INFINITE_RANK;
  }
  min_hoprankinc = p->dag->instance->min_hoprankinc;
  return (RANK_FACTOR * STEP_OF_RANK(p) + RANK_STRETCH) * min_hoprankinc;
}
/*---------------------------------------------------------------------------*/
static uint16_t
parent_path_cost(rpl_parent_t *p)
{
  if(p == NULL) {
    return 0xffff;
  }
  /* path cost upper bound: 0xffff */
  return MIN((uint32_t)p->rank + parent_link_metric(p), 0xffff);
}
/*---------------------------------------------------------------------------*/
static rpl_rank_t
rank_via_parent(rpl_parent_t *p)
{
  if(p == NULL) {
    return INFINITE_RANK;
  } else {
    return MIN((uint32_t)p->rank + parent_rank_increase(p), INFINITE_RANK);
  }
}
/*---------------------------------------------------------------------------*/
static int
parent_is_acceptable(rpl_parent_t *p)
{
  return STEP_OF_RANK(p) >= MIN_STEP_OF_RANK
      && STEP_OF_RANK(p) <= MAX_STEP_OF_RANK;
}
/*---------------------------------------------------------------------------*/
static int
parent_has_usable_link(rpl_parent_t *p)
{
  return parent_is_acceptable(p);
}
/*---------------------------------------------------------------------------*/
	static rpl_parent_t *
	best_parent(rpl_parent_t *p1, rpl_parent_t *p2)
	{
	  rpl_rank_t r1, r2;
	  rpl_dag_t *dag;
	  
	  PRINTF("RPL: Comparing parent ");
	  PRINT6ADDR(rpl_get_parent_ipaddr(p1));
	  PRINTF(" (confidence %d, rank %d) with parent ",
	        p1->link_metric, p1->rank);
	  PRINT6ADDR(rpl_get_parent_ipaddr(p2));
	  PRINTF(" (confidence %d, rank %d)\n",
	        p2->link_metric, p2->rank);
	
	
	
	
	  r1 = DAG_RANK(p1->rank, p1->dag->instance) * RPL_MIN_HOPRANKINC  +
	         p1->link_metric;
	  r2 = DAG_RANK(p2->rank, p1->dag->instance) * RPL_MIN_HOPRANKINC  +
	         p2->link_metric;
	  /* Compare two parents by looking both and their rank and at the ETX
	     for that parent. We choose the parent that has the most
	     favourable combination. */
	
	
	
	
	  dag = (rpl_dag_t *)p1->dag; /* Both parents must be in the same DAG. */
	  if(r1 < r2 + MIN_DIFFERENCE &&
	     r1 > r2 - MIN_DIFFERENCE) {
	    return dag->preferred_parent;
	  } else if(r1 < r2) {
	    return p1;
	  } else {
	    return p2;
	
	
	
	
	
	
	
	  }
	}
/*---------------------------------------------------------------------------*/
	static rpl_dag_t *
	best_dag(rpl_dag_t *d1, rpl_dag_t *d2)
	
	{
	  if(d1->grounded) {
	    if (!d2->grounded) {
	      return d1;
	    }
	  } else if(d2->grounded) {
	    return d2;

	  }
	
	  if(d1->preference < d2->preference) {
	    return d2;
	  } else {
	    if(d1->preference > d2->preference) {
	      return d1;
	    }
	
	
	  }
	
	  if(d2->rank < d1->rank) {
	    return d2;
	
	  } else {
	    return d1;
	  }
	}


/** @}*/
