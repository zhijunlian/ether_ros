/******************************************************************************
 *
 *  $Id$
 *
 *  Copyright (C) 2018 Mike Karamousadakis, NTUA CSL
 *
 *  This file is part of the IgH EtherCAT master userspace program in the ROS environment.
 *
 *  The IgH EtherCAT master userspace program in the ROS environment is free software; you can
 *  redistribute it and/or modify it under the terms of the GNU General
 *  Public License as published by the Free Software Foundation; version 2
 *  of the License.
 *
 *  The IgH EtherCAT master userspace program in the ROS environment is distributed in the hope that
 *  it will be useful, but WITHOUT ANY WARRANTY; without even the implied
 *  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with the IgH EtherCAT master userspace program in the ROS environment. If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 *  ---
 *
 *  The license mentioned above concerns the source code only. Using the
 *  EtherCAT technology and brand is only permitted in compliance with the
 *  industrial property and similar rights of Beckhoff Automation GmbH.
 *
 *  Contact information: mkaramousadakis@zoho.eu
 *****************************************************************************/
/**
   \file ethercat_communicator.cpp
   \brief Implementation of EthercatCommunicator class.

   Used for real-time communication with the EtherCAT slaves, via the IgH Master module. The new PD are sent
   to the  \a /ethercat_data_raw topic.
*/

/*****************************************************************************/
#include "ethercat_communicator.h"
#include "utilities.h"
#include "ethercat_slave.h"
#include "ether_ros.h"
#include "ether_ros/PDORaw.h"
#include "deadline_scheduler.h"

int EthercatCommunicator::cleanup_pop_arg_ = 0;
bool EthercatCommunicator::running_thread_ = false;
pthread_t EthercatCommunicator::communicator_thread_ = {};
ros::Publisher EthercatCommunicator::pdo_raw_pub_;

uint64_t EthercatCommunicator::dc_start_time_ns_ = 0LL;
uint64_t EthercatCommunicator::dc_time_ns_ = 0;
int64_t EthercatCommunicator::system_time_base_ = 0LL;
#ifdef SYNC_MASTER_TO_REF
    uint8_t EthercatCommunicator::dc_started_ = 0;
    int32_t EthercatCommunicator::dc_diff_ns_ = 0;
    int32_t EthercatCommunicator::prev_dc_diff_ns_ = 0;
    int64_t EthercatCommunicator::dc_diff_total_ns_ = 0LL;
    int64_t EthercatCommunicator::dc_delta_total_ns_ = 0LL;
    int EthercatCommunicator::dc_filter_idx_ = 0;
    int64_t EthercatCommunicator::dc_adjust_ns_;
#endif
#ifdef LOGGING
    statistics_struct stat_struct;
#endif
//--------------------------------------------------------------------------//

/** Get the time in ns for the current cpu, adjusted by system_time_base_.
 *
 * \attention Rather than calling rt_get_time_ns() directly, all application
 * time calls should use this method instead.
 *
 * \ret The time in ns.
 */
uint64_t EthercatCommunicator::system_time_ns(void)
{
    struct timespec time;
    uint64_t time_ns;
    clock_gettime(CLOCK_TO_USE, &time);

    time_ns = TIMESPEC2NS(time);

    if (system_time_base_ > (int64_t)time_ns)
    {
        ROS_INFO(" system_time_ns error: system_time_base_ greater than"
                  " system time (system_time_base_: %ld, time: %lu\n",
                   system_time_base_, (int64_t) time_ns);
        return time_ns;
    }
    else
    {
        return time_ns - (uint64_t) system_time_base_;
    }
}

//--------------------------------------------------------------------------//

/** Synchronise the distributed clocks
 */
void EthercatCommunicator::sync_distributed_clocks(void)
{
#ifdef SYNC_MASTER_TO_REF
    uint32_t ref_time = 0;
    uint64_t prev_app_time = dc_time_ns_;
#endif

    dc_time_ns_ = system_time_ns();

    // set master time in nano-seconds
    ecrt_master_application_time(master, dc_time_ns_);

#ifdef SYNC_MASTER_TO_REF
    // get reference clock time to synchronize master cycle
    ecrt_master_reference_clock_time(master, &ref_time);
    dc_diff_ns_ = (uint32_t)prev_app_time - ref_time;
#endif
#ifdef SYNC_REF_TO_MASTER
    // sync reference clock to master
    ecrt_master_sync_reference_clock(master);
#endif

    // call to sync slaves to ref slave
    ecrt_master_sync_slave_clocks(master);
}

//--------------------------------------------------------------------------//

/** Update the master time based on ref slaves time diff
 *
 * called after the ethercat frame is sent to avoid time jitter in
 * sync_distributed_clocks()
 */
void EthercatCommunicator::update_master_clock(void)
{
#ifdef SYNC_MASTER_TO_REF
    // calc drift (via un-normalised time diff)
    int32_t delta = dc_diff_ns_ - prev_dc_diff_ns_;
    prev_dc_diff_ns_ = dc_diff_ns_;

    // normalise the time diff
    dc_diff_ns_ =
        ((dc_diff_ns_ + (PERIOD_NS / 2)) % PERIOD_NS) - (PERIOD_NS / 2);

    // only update if primary master
    if (dc_started_)
    {

        // add to totals
        dc_diff_total_ns_ += dc_diff_ns_;
        dc_delta_total_ns_ += delta;
        dc_filter_idx_++;

        if (dc_filter_idx_ >= DC_FILTER_CNT)
        {
            // add rounded delta average
            dc_adjust_ns_ +=
                ((dc_delta_total_ns_ + (DC_FILTER_CNT / 2)) / DC_FILTER_CNT);

            // and add adjustment for general diff (to pull in drift)
            dc_adjust_ns_ += sign(dc_diff_total_ns_ / DC_FILTER_CNT);

            // limit crazy numbers (0.1% of std cycle time)
            if (dc_adjust_ns_ < -1000)
            {
                dc_adjust_ns_ = -1000;
            }
            if (dc_adjust_ns_ > 1000)
            {
                dc_adjust_ns_ = 1000;
            }

            // reset
            dc_diff_total_ns_ = 0LL;
            dc_delta_total_ns_ = 0LL;
            dc_filter_idx_ = 0;
        }

        // add cycles adjustment to time base (including a spot adjustment)
        system_time_base_ += dc_adjust_ns_ + sign(dc_diff_ns_);
    }
    else
    {
        dc_started_ = (dc_diff_ns_ != 0);

        if (dc_started_)
        {
            // output first diff
            ROS_INFO("First master diff: %d.\n", dc_diff_ns_);

            // record the time of this initial cycle
            dc_start_time_ns_ = dc_time_ns_;
        }
    }
#endif
}

//--------------------------------------------------------------------------//

bool EthercatCommunicator::has_running_thread()
{
    return running_thread_;
}
//--------------------------------------------------------------------------//
void EthercatCommunicator::init(ros::NodeHandle &n)
{

    const struct sched_param sched_param_ = {.sched_priority = 80};
    struct sched_param act_param = {};
    int act_policy;
    int ret;

    /******************************************
     * Initialize the timing sampling buffers.
    *******************************************/
#ifdef LOGGING
#ifdef LOGGING_SAMPLING
    statistics_struct stat_struct = {0, 0, 0, 0};
    stat_struct.latency_min_ns = (uint32_t *)malloc(RUN_TIME * SAMPLING_FREQ * (sizeof(uint32_t)));
    stat_struct.latency_max_ns = (uint32_t *)malloc(RUN_TIME * SAMPLING_FREQ * (sizeof(uint32_t)));
    stat_struct.period_min_ns = (uint32_t *)malloc(RUN_TIME * SAMPLING_FREQ * (sizeof(uint32_t)));
    stat_struct.period_max_ns = (uint32_t *)malloc(RUN_TIME * SAMPLING_FREQ * (sizeof(uint32_t)));
    stat_struct.exec_min_ns = (uint32_t *)malloc(RUN_TIME * SAMPLING_FREQ * (sizeof(uint32_t)));
    stat_struct.exec_max_ns = (uint32_t *)malloc(RUN_TIME * SAMPLING_FREQ * (sizeof(uint32_t)));
#endif
#ifdef LOGGING_NO_SAMPLING
    statistics_struct stat_struct = {0};
    stat_struct.latency_ns = (uint32_t *)malloc(RUN_TIME * FREQUENCY * (sizeof(uint32_t)));
    stat_struct.period_ns = (uint32_t *)malloc(RUN_TIME * FREQUENCY * (sizeof(uint32_t)));
    stat_struct.exec_ns = (uint32_t *)malloc(RUN_TIME * FREQUENCY * (sizeof(uint32_t)));
#endif
#endif

    if (pthread_attr_init(&current_thattr_))
    {
        ROS_FATAL("Attribute init\n");
        exit(1);
    }
    if (pthread_attr_setdetachstate(&current_thattr_, PTHREAD_CREATE_JOINABLE))
    {
        ROS_FATAL("Attribute set detach state\n");
        exit(1);
    }
    if (pthread_attr_setinheritsched(&current_thattr_, PTHREAD_EXPLICIT_SCHED))
    {
        ROS_FATAL("Attribute set inherit schedule\n");
        exit(1);
    }

    /*
    * Use the SCHED_FIFO for now. It should be tested later if there is a better scheduler (see: SCHED_DEADLINE, EDF + CBS)
    */
    if (pthread_attr_setschedpolicy(&current_thattr_, SCHED_FIFO))
    {
        ROS_FATAL("Attribute set schedule policy\n");
        exit(1);
    }
    ret = pthread_attr_setschedparam(&current_thattr_, &sched_param_);
    if (ret != 0)
    {
        handle_error_en(ret, "pthread_attr_setschedparam");
    }
    // Get the values we just set, to make sure that they are set
    ret = pthread_attr_getschedparam(&current_thattr_, &act_param);
    if (ret != 0)
    {
        handle_error_en(ret, "pthread_attr_getschedparam");
    }
    ret = pthread_attr_getschedpolicy(&current_thattr_, &act_policy);
    if (ret != 0)
    {
        handle_error_en(ret, "pthread_attr_getschedpolicy");
    }
    ROS_WARN("Actual pthread attribute values are: %d , %d\n", act_policy, act_param.sched_priority);

    //Create  ROS publisher for the Ethercat RAW data
    pdo_raw_pub_ = n.advertise<ether_ros::PDORaw>("pdo_raw", 1000);


}
//--------------------------------------------------------------------------//
void EthercatCommunicator::start()
{
    int ret;
#ifdef SYNC_MASTER_TO_REF
    /* Set the initial master time and select a slave to use as the DC
     * reference clock, otherwise pass NULL to auto select the first capable
     * slave. Note: This can be used whether the master or the ref slave will
     * be used as the systems master DC clock.
     */
    // dc_start_time_ns_ = system_time_ns();
    // dc_time_ns_ = dc_start_time_ns_;

    /* Attention: The initial application time is also used for phase
     * calculation for the SYNC0/1 interrupts. Please be sure to call it at
     * the correct phase to the realtime cycle.
     */
    // ecrt_master_application_time(master, dc_start_time_ns_);
#endif
    ret = ecrt_master_select_reference_clock(master, ethercat_slaves[0].slave.get_slave_config());
    if (ret < 0)
    {
        handle_error_en(ret, "Failed to select reference clock. \n");
    }

    ROS_INFO("Activating master...\n");
    if (ecrt_master_activate(master))
    {
        ROS_FATAL("Failed to activate master.\n");
        exit(1);
    }
    domain1_pd = NULL;
    if (!(domain1_pd = ecrt_domain_data(domain1)))
    {
        ROS_FATAL("Failed to set domain data.\n");
        exit(1);
    }
    running_thread_ = true;

    ret = pthread_create(&communicator_thread_, &current_thattr_, &EthercatCommunicator::run, NULL);
    if (ret != 0)
    {
        handle_error_en(ret, "pthread_create");
    }
    ROS_INFO("Starting cyclic thread.\n");
}
//--------------------------------------------------------------------------//
void EthercatCommunicator::cleanup_handler(void *arg)
{
    ROS_INFO("Called clean-up handler\n");
}
//--------------------------------------------------------------------------//
#ifdef LOGGING
void EthercatCommunicator::log_statistics_to_file(statistics_struct *ss)
{
    int i;
    char log_string[100];
#ifdef LOGGING_SAMPLING
    for (i = 0; i < RUN_TIME * SAMPLING_FREQ; i++)
    {
        snprintf(log_string, 100, "%10u , %10u , 10u , %10u , 10u , %10u\n",
                 ss->period_min_ns[i], ss->period_max_ns[i], ss->exec_min_ns[i], ss->exec_max_ns[i],
                 ss->latency_min_ns[i], ss->latency_max_ns[i]);
        dprintf(log_fd, "%s", log_string);
    }
#endif
#ifdef LOGGING_NO_SAMPLING
    for (i = 0; i < RUN_TIME * FREQUENCY; i++)
    {
        if (i % 10000 == 0)
            ROS_INFO("Current line written is: %d\n", i);
        snprintf(log_string, sizeof(log_string), "%10u , %10u , %10u\n",
                 ss->period_ns[i], ss->exec_ns[i], ss->latency_ns[i]);
        if ((uint32_t)utilities::insist_write(log_fd, log_string, strlen(log_string)) != strlen(log_string))
        {
            ROS_FATAL("ec_thread: insist_write");
            exit(1);
        }
    }
#endif

    if (close(log_fd))
    {
        ROS_ERROR("ec_thread: close log fd");
        exit(1);
    }
}
#endif
//--------------------------------------------------------------------------//
#ifdef LOGGING
void EthercatCommunicator::create_statistics(statistics_struct * ss, struct timespec * wakeup_time_p)
{
#ifdef LOGGING_SAMPLING
    ss->latency_ns = DIFF_NS(*wakeup_time_p, ss->start_time);
    ss->period_ns = DIFF_NS(ss->last_start_time, ss->start_time);
    ss->exec_ns = DIFF_NS(ss->last_start_time, ss->end_time);

    if (ss->latency_ns > ss->latency_max_ns[ss->statistics_id])
    {
        ss->latency_max_ns[ss->statistics_id] = ss->latency_ns;
    }
    if (ss->latency_ns < ss->latency_min_ns[ss->statistics_id])
    {
        ss->latency_min_ns[ss->statistics_id] = ss->latency_ns;
    }
    if (ss->period_ns > ss->period_max_ns[ss->statistics_id])
    {
        ss->period_max_ns[ss->statistics_id] = ss->period_ns;
    }
    if (ss->period_ns < ss->period_min_ns[ss->statistics_id])
    {
        ss->period_min_ns[ss->statistics_id] = ss->period_ns;
    }
    if (ss->exec_ns > ss->exec_max_ns[ss->statistics_id])
    {
        ss->exec_max_ns[ss->statistics_id] = ss->exec_ns;
    }
    if (ss->exec_ns < ss->exec_min_ns[ss->statistics_id])
    {
        ss->exec_min_ns[ss->statistics_id] = ss->exec_ns;
    }
#ifdef LOGGING_NO_SAMPLING
    ss->latency_ns[ss->statistics_id] = DIFF_NS(*wakeup_time_p, ss->start_time);
    ss->period_ns[ss->statistics_id] = DIFF_NS(ss->last_start_time, ss->start_time);
    ss->exec_ns[ss->statistics_id] = DIFF_NS(ss->last_start_time, ss->end_time);
#endif
#endif
}
#endif
//--------------------------------------------------------------------------//
#ifdef LOGGING
void EthercatCommunicator::create_new_statistics_sample(statistics_struct *ss, unsigned int * sampling_counter)
{

#ifdef LOGGING_SAMPLING
    *sampling_counter = FREQUENCY / SAMPLING_FREQ;
#endif
#ifdef LOGGING_NO_SAMPLING
    *sampling_counter = 0;
#endif
    ss->statistics_id++;


#if defined(LOGGING) && defined(LOGGING_SAMPLING)
    // output timing stats
    // printf("period     %10u ... %10u\n",
    //         period_min_ns, period_max_ns);
    // printf("exec       %10u ... %10u\n",
    //         exec_min_ns, exec_max_ns);
    // printf("latency    %10u ... %10u\n",
    //         latency_min_ns, latency_max_ns);

    period_max_ns[ss->statistics_id] = 0;
    period_min_ns[ss->statistics_id] = 0xffffffff;
    exec_max_ns[ss->statistics_id] = 0;
    exec_min_ns[ss->statistics_id] = 0xffffffff;
    latency_max_ns[ss->statistics_id] = 0;
    latency_min_ns[ss->statistics_id] = 0xffffffff;
#endif
}
#endif

//--------------------------------------------------------------------------//
void *EthercatCommunicator::run(void *arg)
{
    pthread_cleanup_push(EthercatCommunicator::cleanup_handler, NULL);

    unsigned int sampling_counter = 0;
    unsigned int sync_ref_counter = 0;
    const struct timespec cycletime = {0, PERIOD_NS};
    struct timespec break_time, current_time, offset_time = {RUN_TIME, 0}, wakeup_time;
    int ret;
    cpu_set_t cpuset_;
#ifdef DEADLINE_SCHEDULING
    struct sched_attr sched_attr_;
    sched_attr_.size = sizeof(struct sched_attr);
    sched_attr_.sched_policy = SCHED_DEADLINE;
    sched_attr_.sched_priority = 0;
    sched_attr_.sched_runtime = 30000;
    sched_attr_.sched_deadline = 100000;
    sched_attr_.sched_period = PERIOD_NS;
#endif
#ifdef FIFO_SCHEDULING
    CPU_SET(3, &cpuset_);

    // set pthread affinity to CPU 3
    if (pthread_setaffinity_np(communicator_thread_, sizeof(cpuset_), &cpuset_))
    {
        ROS_FATAL("Set pthread affinity, not portable\n");
        exit(1);
    }
#endif
#ifdef DEADLINE_SCHEDULING
    ROS_INFO("Size: %d, Policy: %u, Priority: %u, Runtime: %llu, Deadline: %llu, Period: %llu",
             sched_attr_.size, sched_attr_.sched_policy,
             sched_attr_.sched_priority, sched_attr_.sched_runtime, sched_attr_.sched_deadline, sched_attr_.sched_period);
    if (sched_setattr(0, &sched_attr_, 0))
    {
        ROS_FATAL("Set schedule attributes for DEADLINE scheduling\n");
        exit(1);
    }
#endif
    // get current time
    clock_gettime(CLOCK_TO_USE, &wakeup_time);
    clock_gettime(CLOCK_TO_USE, &break_time);
    break_time = utilities::timespec_add(break_time, offset_time);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL); //PTHREAD_CANCEL_DEFERRED is the default but nevertheless

    do
    {
        // check if there is a request for cancel
        pthread_testcancel();
        //set the cancel state to DISABLE
        ret = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
        if (ret != 0)
        {
            handle_error_en(ret, "pthread_setcancelstate");
        }
        wakeup_time = utilities::timespec_add(wakeup_time, cycletime);
        clock_nanosleep(CLOCK_TO_USE, TIMER_ABSTIME, &wakeup_time, NULL);
#ifdef LOGGING
        clock_gettime(CLOCK_TO_USE, & stat_struct.start_time);
        create_statistics(&stat_struct, &wakeup_time);
        stat_struct.last_start_time = stat_struct.start_time;
#endif

        // receive EtherCAT frame
        ecrt_master_receive(master);
        // receive process data
        ecrt_domain_process(domain1);
        // check the state of the domain
        utilities::check_domain1_state();

        // get statistics if the flags are enabled
        if (!sampling_counter) //if sampling_counter is 0
        {
            // do this at 10 Hz
        #ifdef LOGGING
            create_new_statistics_sample(&stat_struct, &sampling_counter);
        #endif
            // check for master state (optional)
            utilities::check_master_state();
        }
        else sampling_counter--;

        // move the data from process_data_buf to domain1_pd buf carefuly
        utilities::copy_process_data_buffer_to_buf(domain1_pd);


        // queue the EtherCAT data to domain buffer
        ecrt_domain_queue(domain1);

        // sync distributed clock just before master_send to set
        // most accurate master clock time. The two modes MASTER2REF and REF2MASTER should be supported.
        // However if the REF2MASTER doesn't work for some reason, comment the following line and comment out the
        // following ones.
        EthercatCommunicator::sync_distributed_clocks();

        // write application time to master
#ifdef SYNC_REF_TO_MASTER
        // clock_gettime(CLOCK_TO_USE, &current_time);
        // ecrt_master_application_time(master, TIMESPEC2NS(current_time));

        // ecrt_master_sync_reference_clock(master);
        // ecrt_master_sync_slave_clocks(master);
#endif
        // send EtherCAT frame
        ecrt_master_send(master);

        // send the raw data to the raw data topic
        EthercatCommunicator::publish_raw_data();
        // update the master clock with the drift, if SYNC_MASTER_TO_REF defined
        EthercatCommunicator::update_master_clock();
        int ret = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL); //set the cancel state to ENABLE
        if (ret != 0)
        {
            handle_error_en(ret, "pthread_setcancelstate");
        }
#ifdef LOGGING
        clock_gettime(CLOCK_TO_USE, & stat_struct.end_time);
#endif
        clock_gettime(CLOCK_TO_USE, &current_time);
    } while (DIFF_NS(current_time, break_time) > 0);

#ifdef LOGGING
    // write the statistics to file
    log_statistics_to_file();
#endif
    pthread_cleanup_pop(cleanup_pop_arg_);
    running_thread_ = false;
    exit(0);
}
//--------------------------------------------------------------------------//
void EthercatCommunicator::stop()
{

    int ret;
    void *res;

    ROS_INFO("stop(): sending cancellation request\n");
    ret = pthread_cancel(communicator_thread_);
    if (ret != 0)
        handle_error_en(ret, "pthread_cancel");

    /* Join with thread to see what its exit status was */

    ret = pthread_join(communicator_thread_, &res);
    // ecrt_master_deactivate_slaves(master);

    memset(process_data_buf, 0, total_process_data); // fill the buffer with zeros
    if (ret != 0)
        handle_error_en(ret, "pthread_join");

    if (res == PTHREAD_CANCELED)
    {
        ROS_INFO("stop(): communicator thread  was canceled\n");
        running_thread_ = false;
    }
    else
        ROS_INFO("stop(): communicator thread wasn't canceled (shouldn't happen!)\n");
}
//--------------------------------------------------------------------------//
void EthercatCommunicator::publish_raw_data()
{
    std::vector<uint8_t> input_data_raw, output_data_raw;
    //Create input data raw string
    std::vector<uint8_t> input_vec, output_vec;
    unsigned char *raw_data_pointer;
    for (int i = 0; i < master_info.slave_count; i++)
    {
        raw_data_pointer = (unsigned char *)domain1_pd + ethercat_slaves[i].slave.get_pdo_in();
        input_vec.insert(std::end(input_vec), raw_data_pointer, raw_data_pointer + num_process_data_in);
    }
    input_data_raw.insert(std::end(input_data_raw), std::begin(input_vec), std::end(input_vec));
    //Create output data raw string
    for (int i = 0; i < master_info.slave_count; i++)
    {
        raw_data_pointer = (unsigned char *)domain1_pd + ethercat_slaves[i].slave.get_pdo_out();
        output_vec.insert(std::end(output_vec), raw_data_pointer, raw_data_pointer + num_process_data_out);
    }
    output_data_raw.insert(std::end(output_data_raw), std::begin(output_vec), std::end(output_vec));
    //Send both strings to the topic
    ether_ros::PDORaw raw_data;
    raw_data.pdo_in_raw = input_data_raw;
    raw_data.pdo_out_raw = output_data_raw;
    pdo_raw_pub_.publish(raw_data);
}
