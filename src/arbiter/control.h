/**
 * Experiment control
 */

#ifndef CONTROL_H_
#define CONTROL_H_

#include <rte_ip.h>
#include <stdint.h>
#include "../graph-algo/algo_config.h"

#define I_AM_MASTER				1
#define IS_STRESS_TEST			0

#define STRESS_TEST_IS_AUTOMATED        1
#define STRESS_TEST_MEAN_T_BETWEEN_REQUESTS_SEC		.5e-3
#define STRESS_TEST_NUM_NODES						32
#define STRESS_TEST_DEMAND_TSLOTS					10
#define STRESS_TEST_DURATION_SEC					70
#define STRESS_TEST_RATE_INCREASE_FACTOR			2.0
#define STRESS_TEST_RATE_INCREASE_GAP_SEC			2
#define STRESS_TEST_INITIAL_SOURCES					10
#define STRESS_TEST_INITIAL_DSTS_PER_SRC			1
#define STRESS_TEST_INITIAL_FLOW_SIZE				(10)
#define STRESS_TEST_START_GAP_SEC				1

/* bits 1-3 occupied by other experiments */
#define LAUNCH_CONTROLLER_EXPT 			0x8
#define LAUNCH_LOCAL_STRESS_TEST_EXPT	0x10

/* don't set up ports for stress test */
#if IS_STRESS_TEST == 1
#define EXPT_RUN_MASK 0
#define N_CONTROLLER_PORTS		0
#else
#define EXPT_RUN_MASK (LAUNCH_CONTROLLER_EXPT)
#define N_CONTROLLER_PORTS		1
#endif

#define N_ADMISSION_CORES		ALGO_N_CORES
#define N_PATH_SEL_CORES		0
#define N_COMM_CORES			1
#define N_LOG_CORES				1

/* Core indices */
#define FIRST_COMM_CORE			0
#define FIRST_ADMISSION_CORE	(FIRST_COMM_CORE + N_COMM_CORES)
#define FIRST_PATH_SEL_CORE		(FIRST_ADMISSION_CORE + N_ADMISSION_CORES)
#define FIRST_LOG_CORE			(FIRST_PATH_SEL_CORE + N_PATH_SEL_CORES)


#define NUM_RACKS				1

/* how many timeslots before allocated timeslot to start processing it */
#define		PREALLOC_DURATION_TIMESLOTS		40

/* getting timeslot from time is ((NOW_NS * MUL) >> SHIFT) */
#define		TIMESLOT_MUL		419
#define		TIMESLOT_SHIFT		19

/* give the controller some time to initialize before starting allocation */
#define		INIT_MAX_TIME_NS		(200*1000*1000)

/* how many seconds in between writes to log */
#define		LOG_GAP_SECS		0.1

#define RTE_LOGTYPE_CONTROL RTE_LOGTYPE_USER1
#define CONTROL_DEBUG(a...) RTE_LOG(DEBUG, CONTROL, ##a)
#define CONTROL_INFO(a...) RTE_LOG(INFO, CONTROL, ##a)

/**
 * Allocate queues to lcores
 */
int control_do_queue_allocation(void);

/**
 * Performs experiments
 */
void launch_cores(void);


#endif /* CONTROL_H_ */
