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
   \file ethercat_slave.cpp
   \brief Implementation of EthercatSlave class.

   Used for containing all the useful information
   of an EtherCAT slave, from the userspace program perspective. Receives all the
   useful information via the ROS Parameter Server (after they are loaded from ethercat_slaves.yaml).
*/

/*****************************************************************************/
#include <iostream>
#include "ethercat_slave.h"
#include "ether_ros.h"

void EthercatSlave::init(std::string slave, ros::NodeHandle &n)
{

    slave_id_ = slave;
    std::string slave_root_loc = std::string("/ethercat_slaves/") + slave + "/";

    while (!n.getParam(slave_root_loc + "vendor_id", vendor_id_))
    {
        ROS_INFO("Waiting the parameter server to initialize\n");
    }
    ROS_INFO("Got param: slave_root_loc + vendor_id = %2.2x\n", vendor_id_);

    if (n.getParam(slave_root_loc + "alias", alias_))
    {
        ROS_INFO("Got param: slave_root_loc + alias = %d\n", alias_);
    }
    else
    {
        ROS_FATAL("Failed to get param 'slave_root_loc + alias'\n");
    }

    if (n.getParam(slave_root_loc + "position", position_))
    {
        ROS_INFO("Got param: slave_root_loc + position = %d\n", position_);
    }
    else
    {
        ROS_FATAL("Failed to get param 'slave_root_loc + position'\n");
    }

    if (n.getParam(slave_root_loc + "product_code", product_code_))
    {
        ROS_INFO("Got param: slave_root_loc + product_code = %2.2x\n", product_code_);
    }
    else
    {
        ROS_FATAL("Failed to get param 'slave_root_loc + product_code'\n");
    }

    if (n.getParam(slave_root_loc + "assign_activate", assign_activate_))
    {
        ROS_INFO("Got param: slave_root_loc + assign_activate = %2.2x\n", assign_activate_);
    }
    else
    {
        ROS_FATAL("Failed to get param 'slave_root_loc + assign_activate'\n");
    }

    if (n.getParam(slave_root_loc + "input_port", input_port_))
    {
        ROS_INFO("Got param: slave_root_loc + input_port = %2.2x\n", input_port_);
    }
    else
    {
        ROS_FATAL("Failed to get param 'slave_root_loc + input_port'\n");
    }

    if (n.getParam(slave_root_loc + "output_port", output_port_))
    {
        ROS_INFO("Got param: slave_root_loc + output_port = %2.2x\n", output_port_);
    }
    else
    {
        ROS_FATAL("Failed to get param 'slave_root_loc + output_port'\n");
    }
    ethercat_slave_ = ecrt_master_slave_config(master, alias_, position_, vendor_id_, product_code_);
    if (!ethercat_slave_)
    {
        ROS_FATAL("Failed to get slave configuration.\n");
        exit(1);
    }
    pdo_out_ = ecrt_slave_config_reg_pdo_entry(ethercat_slave_, output_port_, 1, domain1, NULL);
    if (pdo_out_ < 0)
    {
        ROS_FATAL("Failed to configure pdo out.\n");
        exit(1);
    }
    ROS_INFO("Offset pdo out is: %d\n", pdo_out_);

    pdo_in_ = ecrt_slave_config_reg_pdo_entry(ethercat_slave_, input_port_, 1, domain1, NULL);
    if (pdo_in_ < 0)
    {
        ROS_FATAL("Failed to configure pdo in.\n");
        exit(1);
    }
    ROS_INFO("Offset pdo in is: %d\n", pdo_in_);

    if (n.getParam("/ethercat_slaves/sync0_shift", sync0_shift_))
    {
        ROS_INFO("Got param: /ethercat_slaves/sync0_shift = %2.2x\n", sync0_shift_);
    }
    else
    {
        ROS_FATAL("Failed to get param '/ethercat_slaves/sync0_shift'\n");
    }
    // configure SYNC signals for this slave
    //For XMC use: 0x0300
    //For Beckhoff FB1111 use: 0x0700
    //Use PERIOD_NS as the period, and 50 μs shift time
    ecrt_slave_config_dc(ethercat_slave_, assign_activate_, PERIOD_NS, sync0_shift_, 0, 0);
}

int EthercatSlave::get_pdo_in()
{
    return pdo_in_;
}

int EthercatSlave::get_pdo_out()
{
    return pdo_out_;
}
ec_slave_config_t *EthercatSlave::get_slave_config()
{
    return ethercat_slave_;
}
