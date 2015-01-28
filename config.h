/*
 * config.h
 *
 *  Created on: June 23, 2014
 *      Author: aousterh
 */

#ifndef CONFIG_H_
#define CONFIG_H_

#define EMU_RACK_SHIFT			5
#define EMU_ENDPOINTS_PER_RACK	(1 << EMU_RACK_SHIFT)
#define EMU_ENDPOINTS_PER_EPG	(EMU_ENDPOINTS_PER_RACK)
#define EMU_ADMITS_PER_ADMITTED	(2 * EMU_NUM_ENDPOINTS)
#define EMU_MAX_OUTPUTS_PER_RTR	2

#define SINGLE_RACK_TOPOLOGY
//#define TWO_RACK_TOPOLOGY

#ifndef ALGO_N_CORES
#define ALGO_N_CORES			2
//#define ALGO_N_CORES			5
#endif

#ifdef SINGLE_RACK_TOPOLOGY

#define EMU_NUM_ROUTERS			1
#define EMU_NUM_ENDPOINTS		(EMU_ENDPOINTS_PER_RACK * EMU_NUM_ROUTERS)
#define EMU_NUM_ENDPOINT_GROUPS	1

#elif defined(TWO_RACK_TOPOLOGY)

#define EMU_NUM_TORS			2
#define EMU_NUM_CORE_ROUTERS	1
#define EMU_NUM_ROUTERS			(EMU_NUM_TORS + EMU_NUM_CORE_ROUTERS)
#define EMU_NUM_ENDPOINTS		(EMU_ENDPOINTS_PER_RACK * 2)
#define EMU_NUM_ENDPOINT_GROUPS	(EMU_NUM_TORS)

#endif

/* comm core state - 1 comm core right now */
#define EPGS_PER_COMM			(EMU_NUM_ENDPOINT_GROUPS)

#endif /* CONFIG_H_ */
