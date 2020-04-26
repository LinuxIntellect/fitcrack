/**
 * @file assimilator.cpp
 * @authors Martin Holkovic
 * @authors Lukas Zobal
 * @date 4. 2. 2017
 * @note Huge refactoring needed
 */

// This file is part of BOINC.
// http://boinc.berkeley.edu
// Copyright (C) 2015 University of California
//
// BOINC is free software; you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.
//
// BOINC is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with BOINC.  If not, see <http://www.gnu.org/licenses/>.

// A sample assimilator that:
// 1) if success, copy the output file(s) to a directory
// 2) if failure, append a message to an error log
//
// This is a sample BOINC work assimilator file edited by
// Martin Holkovic and Lukas Zobal


/**********************************/
/** @section Includes/definitions */
/**********************************/

#include <vector>
#include <map>
#include <string>
#include <cstdlib>
#include <math.h>
#include <boost/lexical_cast.hpp>
#include <iostream>
#include <stdint.h>
#include <inttypes.h>

#include "Config.h"
#include "backend_lib.h"
#include "boinc_db.h"
#include "error_numbers.h"
#include "filesys.h"
#include "sched_msgs.h"
#include "validate_util.h"
#include "sched_config.h"
#include "sched_util.h"
#include "assimilate_handler.h"

#include <AttackPcfgClient.h>


/** Static SQL buffer size used in code */
#define SQL_BUF_SIZE 4096
#define MAX_HASH_SIZE 8192

/** Benchmark is usually much faster than real cracking, shrink the first 2 workunits times-: */
#define FIRST_WU_SHRINK_FACTOR 1

/** Special ID of special bench_all job in fc_job */
#define BENCHALL_JOB_ID 1

const std::array<int, 34> BIN_HASH =
        {2500, 2501, 5200, 5300, 5400, 6211, 6212, 6213, 6221, 6222, 6223, 6231, 6232, 6233, 6241, 6242, 6243, 6600,
         8200, 8800, 9000, 13711, 13712, 13713, 13721, 13722, 13723, 13731, 13732, 13733, 13751, 13752, 13753, 14600};


/*******************************/
/** @section 3-hde table names */
/*******************************/

std::string mysql_table_workunit =      "fc_workunit";
std::string mysql_table_job =           "fc_job";
std::string mysql_table_host =          "fc_host";
std::string mysql_table_host_activity = "fc_host_activtity";
std::string mysql_table_mask =          "fc_mask";
std::string mysql_table_settings =      "fc_settings";
std::string mysql_table_benchmark =     "fc_benchmark";
std::string mysql_table_hash =          "fc_hash";


/**
 * @brief JobStatus enum
 */
enum JobStatus
{
    Job_ready       =  0,   // Job is not running
    Job_finished    =  1,   // Password was found
    Job_exhausted   =  2,   // Keyspace was exhausted, pass not found
    Job_malformed   =  3,   // Job has incorrect input
    Job_timeout     =  4,   // time_end of the job was reached, cracking stopped
    Job_running     = 10,   // Job is running
    Job_validating  = 11,   // not used
    Job_finishing   = 12    // Packge is running, but all workunits has been generated, ending soon
};


/**
 * @brief HostStatus enum
 */
enum HostStatus
{
    Host_benchmark  = 0,
    Host_normal     = 1,
    Host_validation = 2,
    Host_done       = 3,
    Host_error      = 4
};


/*******************************/
/** @section Class definitions */
/*******************************/

/**
 * @brief MysqlWorkunit class definiton
 */
class MysqlWorkunit
{
    public:
        /** Constructor */
        MysqlWorkunit(MYSQL_ROW row);

        /** Table Attributes */
        uint64_t m_id;
        uint64_t m_job_id;
        uint64_t m_workunit_id;
        uint64_t m_host_id;
        uint64_t m_boinc_host_id;

        uint64_t m_start_index;
        uint64_t m_start_index_2;
        uint64_t m_hc_keyspace;
        uint64_t m_mask_id;
        bool     m_duplicated;
        uint64_t m_duplicate;
        bool     m_retry;
        bool     m_finished;
};


/**
 * @brief MysqlWorkunit constructor
 * @param row
 */
MysqlWorkunit::MysqlWorkunit(MYSQL_ROW row)
{
    this->m_id = boost::lexical_cast<uint64_t>(row[0]);
    this->m_job_id = boost::lexical_cast<uint64_t>(row[1]);
    this->m_workunit_id = boost::lexical_cast<uint64_t>(row[2]);
    this->m_host_id = boost::lexical_cast<uint64_t>(row[3]);
    this->m_boinc_host_id = boost::lexical_cast<uint64_t>(row[4]);
    this->m_start_index = boost::lexical_cast<uint64_t>(row[5]);
    this->m_start_index_2 = boost::lexical_cast<uint64_t>(row[6]);
    this->m_hc_keyspace = boost::lexical_cast<uint64_t>(row[7]);
    this->m_mask_id = boost::lexical_cast<uint64_t>(row[9]);
    this->m_duplicated = (row[11][0] == '0') ? true: false;
    this->m_duplicate = boost::lexical_cast<uint64_t>(row[12]);
    this->m_retry = (row[15][0] == '0') ? true: false;
    this->m_finished = (row[16][0] == '0') ? true: false;
}


/*****************************************/
/** @section Global function definitions */
/*****************************************/

void update_mysql(char * query)
{
    log_messages.printf(MSG_DEBUG, "update_mysql: %s\n", query);

    int retval = boinc_db.do_query(query);
    if (retval)
    {
        printf ("Problem with db\n");
        boinc_db.close();
        exit(1);
    }
}


uint64_t get_num_from_mysql(char * query)
{
    //std::cerr << __LINE__ << " - Getting number from DB: " << query << std::endl;

    uint64_t result = 0;

    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    update_mysql(query);

    MYSQL_RES* rp;
    rp = mysql_store_result(boinc_db.mysql);
    if (!rp)
    {
        printf ("Problem with db\n");
        boinc_db.close();
        exit(1);
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(rp)))
    {
        if(row[0] != NULL)
            result = boost::lexical_cast<uint64_t>(row[0]);
    }

    mysql_free_result(rp);
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////

    //std::cerr << __LINE__ << " - Result: " << result << std::endl;

    return result;
}

std::string get_str_from_mysql(char * query)
{
    std::cerr << __LINE__ << " - Getting string from DB: " << query << std::endl;

    std::string result = "";

    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    update_mysql(query);

    MYSQL_RES* rp;
    rp = mysql_store_result(boinc_db.mysql);
    if (!rp)
    {
        printf ("Problem with db\n");
        boinc_db.close();
        exit(1);
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(rp)))
    {
        if(row[0] != NULL)
            result = row[0];
    }

    mysql_free_result(rp);
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////

    //std::cerr << __LINE__ << " - Result: " << result << std::endl;

    return result;
}


int write_error(char* p)
{
    static FILE* f = 0;
    if (!f)
    {
        f = fopen(config.project_path("sample_results/errors"), "a");
        if (!f)
            return ERR_FOPEN;
    }

    std::fprintf(f, "%s", p);
    fflush(f);

    return 0;
}


int assimilate_handler_init(int, char**)
{
    return 0;
}


void assimilate_handler_usage()
{
    // describe the project specific arguments here
    //fprintf(stderr,
    //    "    Custom options:\n"
    //    "    [--project_option X]  a project specific option\n"
    //);
}

vector<MysqlWorkunit *> find_workunits2(uint64_t job_id, std::string query)
{
    log_messages.printf(MSG_DEBUG, "find_workunits2: job_id-%" PRIu64 ", query-%s\n", job_id, query.c_str());

    vector<MysqlWorkunit *> result;

    if(query != "")
    {
        query += " AND";
    }

    char buf[SQL_BUF_SIZE];

    std::snprintf(buf, SQL_BUF_SIZE, "SELECT * FROM `%s` WHERE %s host_id IN (SELECT id FROM `%s` WHERE job_id= %" PRIu64 ") ORDER BY id DESC ;", mysql_table_workunit.c_str(), query.c_str(), mysql_table_host.c_str(), job_id);
    update_mysql(buf);

    MYSQL_RES* rp;
    rp = mysql_store_result(boinc_db.mysql);
    if (!rp)
    {
        printf ("Problem with db\n");
        boinc_db.close();
        exit(1);
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(rp)))
    {
        result.push_back(new MysqlWorkunit(row));
    }

    mysql_free_result(rp);

    log_messages.printf(MSG_DEBUG, "find_workunits2-result: %lu workunits\n", result.size());

    return result;
}


// TODO: NOT USBALE
vector<MysqlWorkunit *> find_workunit_duplicates2(uint64_t job_id, uint64_t host_id)
{
    log_messages.printf(MSG_DEBUG, "find_workunit_duplicates2: job_id-%" PRIu64 ", host_id-%" PRIu64 "\n", job_id, host_id);

    vector<MysqlWorkunit *> result, temp;
    char buf[SQL_BUF_SIZE];

    std::snprintf(buf, SQL_BUF_SIZE, "SELECT * FROM `%s` WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_workunit.c_str(), host_id);
    log_messages.printf(MSG_DEBUG, "find_workunit_duplicates2-query: %s\n", buf);

    update_mysql(buf);

    MYSQL_RES* rp;
    rp = mysql_store_result(boinc_db.mysql);
    if (!rp)
    {
        printf ("Problem with db\n");
        boinc_db.close();
        exit(1);
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(rp)))
    {
        result.push_back(new MysqlWorkunit(row));
    }

    mysql_free_result(rp);

    log_messages.printf(MSG_DEBUG, "find_workunit_duplicates2-result init: %lu workunits\n", result.size());

    for (unsigned long int index = 0; index < result.size(); ++index)
    {
        log_messages.printf(MSG_DEBUG, "find_workunit_duplicates2-duplicate1: %lu : %lu workunits\n", index, result.size());

        std::snprintf(buf, SQL_BUF_SIZE, "`duplicate`='%" PRIu64 "'", result[index]->m_id);
        temp = find_workunits2(job_id, buf);

        result.insert(result.end(), temp.begin(), temp.end());

        log_messages.printf(MSG_DEBUG, "find_workunit_duplicates2-duplicate2: %lu : %lu workunits\n", index, result.size());
    }

    return result;
}


void cancel_workunits2(vector<MysqlWorkunit *> workunits)
{
    log_messages.printf(MSG_DEBUG, "cancel_workunits2: %lu workunits\n", workunits.size());

    std::vector<MysqlWorkunit *>::iterator workunit;

    for(workunit = workunits.begin(); workunit != workunits.end(); ++workunit)
    {
        log_messages.printf(MSG_DEBUG, "cancel_workunits: %" PRIu64 "\n", (*workunit)->m_workunit_id);
        //std::cerr << __LINE__ << " cancel workunit-" << (*workunit)->m_workunit_id << std::endl;

        cancel_jobs((*workunit)->m_workunit_id, (*workunit)->m_workunit_id);
    }
}


void delete_workunits2(vector<MysqlWorkunit *> workunits)
{
    log_messages.printf(MSG_DEBUG, "delete_workunits2: workunits:%lu\n", workunits.size());

    std::vector<MysqlWorkunit *>::iterator workunit;
    char buf[SQL_BUF_SIZE];

    for(workunit = workunits.begin(); workunit != workunits.end(); ++workunit)
    {
        std::snprintf(buf, SQL_BUF_SIZE, "DELETE FROM `%s` WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_workunit.c_str(), (*workunit)->m_id);
        update_mysql(buf);
    }
}


/**
 * @brief Sets 'finish' column to number of 'fc_workunit' entries
 * @param workunits Vector of 'fc_workunit' entries
 */
void finish_workunits(vector<MysqlWorkunit *> workunits)
{
    log_messages.printf(MSG_DEBUG, "finish_workunits: workunits:%lu\n", workunits.size());

    std::vector<MysqlWorkunit *>::iterator workunit;
    char buf[SQL_BUF_SIZE];

    for(workunit = workunits.begin(); workunit != workunits.end(); ++workunit)
    {
        std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET finished = 1 WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_workunit.c_str(), (*workunit)->m_id);
        update_mysql(buf);
    }
}


/**
 * @brief Set 'retry' column to one workunit entry
 * @param workunit_id the BOINC workunit ID used for workunit identification
 */
void set_retry_workunit(uint64_t workunit_id)
{
    char buf[SQL_BUF_SIZE];
    std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET retry = 1 WHERE workunit_id = %" PRIu64 " LIMIT 1;", mysql_table_workunit.c_str(), workunit_id);
    update_mysql(buf);
}


/**
 *
 *
 */
void plan_new_benchamrk(uint64_t host_id)
{
    char buf[SQL_BUF_SIZE];

    /** Plan another benchmark in future */
    std::snprintf(buf, SQL_BUF_SIZE, "SELECT power FROM `%s` WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_host.c_str(), host_id);
    uint64_t power = get_num_from_mysql(buf);
    uint64_t minutes = pow(2, power);
    ++power;

    std::cerr << __LINE__ << " - Planning next benchmark in " << minutes << " minutes" << std::endl;

    std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET power = %" PRIu64 ", time = now() + INTERVAL %" PRIu64 " MINUTE WHERE id = %" PRIu64 ";", mysql_table_host.c_str(), power, minutes, host_id);
    update_mysql(buf);
}


void update_power(uint64_t host_id, uint64_t count, double elapsed_time)
{
    char buf[SQL_BUF_SIZE];
    uint64_t power = std::round(count / elapsed_time);
    if(power == 0)
    {
        power = 1;
    }
    //uint64_t actualPower;
    /*
    std::snprintf(buf, SQL_BUF_SIZE, "SELECT `power` FROM `fc_host` WHERE id = %" PRIu64 " LIMIT 1;", host_id);
    power = get_num_from_mysql(buf);
    */

    std::cerr << __LINE__ << " - Updating power: " << power << " to host_id: " << host_id << std::endl;
    std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET power = %" PRIu64 ", time = now() WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_host.c_str(), power, host_id);
    update_mysql(buf);

/*
    // Power is in hc_indexes, not real passwords, we cannot put that to fc_benchmark
    std::cerr << __LINE__ << " - Updating power in fc_benchmark: " << power << " to boinc_host_id: " << boinc_host_id << std::endl;
    std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET `power` = %" PRIu64 ", `last_update` = now() WHERE `boinc_host_id` = %" PRIu64 " AND `hash_type` = %u LIMIT 1 ;",
                                mysql_table_benchmark.c_str(), power, boinc_host_id, hash_type);
    update_mysql(buf);
*/
}


/**
 * @brief Search fc_benchmark table for all entries of current host
 * @param speed_map out Returned map of speeds in format <hash_type, power>
 * @param host_id in Host ID used for searching
 * @return True is map is non-empty, False otherwise
 */
bool find_benchmark_results(std::map<uint32_t, uint64_t> & speed_map, uint64_t boinc_host_id)
{
    uint64_t power;
    uint32_t hash_type;

    char buf[SQL_BUF_SIZE];

    std::snprintf(buf, SQL_BUF_SIZE, "SELECT * FROM `%s` WHERE boinc_host_id = %" PRIu64 " ORDER BY hash_type ASC;", mysql_table_benchmark.c_str(), boinc_host_id);
    log_messages.printf(MSG_DEBUG, "find_benchmark_results-query: %s\n", buf);

    update_mysql(buf);

    MYSQL_RES* rp;
    rp = mysql_store_result(boinc_db.mysql);
    if (!rp)
    {
        printf ("Problem with db\n");
        boinc_db.close();
        exit(1);
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(rp)))
    {
        hash_type = boost::lexical_cast<uint32_t>(row[2]);
        power = boost::lexical_cast<uint64_t>(row[3]);
        speed_map.insert(std::pair<uint32_t, uint64_t>(hash_type, power));
    }

    mysql_free_result(rp);

    if (speed_map.empty())
        return false;
    return true;
}


/**
 * @brief Checks if job has a binary hash type
 * @param job_id ID of the job
 * @return True if job has binary hash_type, False otherwise
 */
bool is_binary(uint64_t job_id)
{
    char buf[SQL_BUF_SIZE];
    std::snprintf(buf, SQL_BUF_SIZE, "SELECT hash_type FROM `%s` WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_job.c_str(), job_id);
    uint64_t hash_type = get_num_from_mysql(buf);

    return (std::find(BIN_HASH.begin(), BIN_HASH.end(), hash_type) != std::end(BIN_HASH));
}


/**
 * @brief Checks if there are any hashes uncracked for the job
 * @param job_id ID of the job
 * @return True if all hashes are cracked (have result), False otherwise
 */
bool no_hashes_left(uint64_t job_id)
{
    char buf[SQL_BUF_SIZE];
    std::snprintf(buf, SQL_BUF_SIZE, "SELECT COUNT(*) FROM `%s` WHERE job_id = %" PRIu64 " AND result IS NULL ;", mysql_table_hash.c_str(), job_id);
    uint64_t hashes_left = get_num_from_mysql(buf);

    return (hashes_left == 0);
}


/********************************/
/** @section Assimilate Handler */
/********************************/

int assimilate_handler(WORKUNIT& wu, vector<RESULT>& /*results*/, RESULT& canonical_result)
{
    std::cerr << std::endl;
    std::cerr << __LINE__ << " - Incoming workunit result with id " << wu.id << std::endl;

    int retval;         // fscanf return values
    char buf[SQL_BUF_SIZE];     // SQL queries
    char mode;          // Result mode (b/n)
    int code;           // Result status code (0 OK, 1 notFound/err, etc.)
    long long unsigned int power; // Host power (bench)
    double cracking_time;       // Cracking time
    char param[256];    // Other string params (err msg)
    char hash_string[MAX_HASH_SIZE];   // hash results

    uint64_t host_id, boinc_host_id, job_id;
    std::vector<MysqlWorkunit *> workunits;

    retval = boinc_mkdir(config.project_path("sample_results"));

    if (retval)
    {
        std::cerr << __LINE__ << " - Failed to create result dir" << std::endl;
        return retval;
    }

    if (!wu.canonical_resultid)
    {
        std::snprintf(buf, SQL_BUF_SIZE, "%s: 0x%x\n", wu.name, wu.error_mask);

        std::cerr << __LINE__ << " - Result error: " << buf;
        std::cerr << std::flush;

        return write_error(buf);
    }

    vector<OUTPUT_FILE_INFO> output_files;
    get_output_file_infos(canonical_result, output_files);
    unsigned long int n = output_files.size();

    /** Find out who sent the result */
    std::snprintf(buf, SQL_BUF_SIZE, "SELECT host_id FROM `%s` WHERE workunit_id = %lu LIMIT 1;", mysql_table_workunit.c_str(), wu.id);
    host_id = get_num_from_mysql(buf);

    std::snprintf(buf, SQL_BUF_SIZE, "SELECT boinc_host_id FROM `%s` WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_host.c_str(), host_id);
    boinc_host_id = get_num_from_mysql(buf);

    std::cerr << __LINE__ << " - From host " << host_id << std::endl;

    /** Find out which job is the result from */
    std::snprintf(buf, SQL_BUF_SIZE, "SELECT job_id FROM `%s` WHERE workunit_id = %lu LIMIT 1;", mysql_table_workunit.c_str(), wu.id);
    job_id = get_num_from_mysql(buf);

    std::cerr << __LINE__ << " - From job " << job_id << std::endl;

    if(!job_id)
    {
        log_messages.printf(MSG_CRITICAL, "not possible to find job for workunit_id = %lu\n", wu.id);

        std::cerr << __LINE__ << " - Error: unable to find such workunit_id - " << wu.id << std::endl;

        return 0;
    }

    // Read the fc_settings
    std::snprintf(buf, SQL_BUF_SIZE, "SELECT delete_finished_workunits FROM `%s` LIMIT 1;", mysql_table_settings.c_str());
    uint64_t deleteFlag = get_num_from_mysql(buf);

    for (unsigned int i = 0; i < n; ++i)
    {
        OUTPUT_FILE_INFO& fi = output_files[i];

        /** Debug output elapsed time */
        std::cerr << __LINE__ << " - Elapsed time: " << canonical_result.elapsed_time << std::endl;

        /** Open the output file */
        FILE* f = std::fopen(fi.path.c_str(), "rb");

        if(f == NULL)
        {
            std::cerr << __LINE__ << " - Error: unable to open output file, code " << errno << std::endl;
            return 0;
        }

        std::cerr << __LINE__ << " - Output file opened: " << fi.path.c_str() << std::endl;

        /** Read the mode */
        retval = std::fscanf(f,"%c\n", &mode);
        if (retval != 1)
        {
            std::cerr << __LINE__ << " - ERROR: Failed to read output mode." << std::endl;
            set_retry_workunit(wu.id);
            std::fclose(f);
            break;
        }

        /** BENCHMARK mode */
        if(mode == 'b')
        {
            std::cerr << __LINE__ << " - WU_MODE: BENCHMARK (b)" << std::endl;
            log_messages.printf(MSG_DEBUG, "mode b\n");

            /** Read the code */
            retval = std::fscanf(f,"%d\n", &code);
            if (retval != 1)
            {
                std::cerr << __LINE__ << " - ERROR: Failed to read status code." << std::endl;
                plan_new_benchamrk(host_id);
                break;
            }

            /** Result OK */
            if(code == 0)
            {
                std::cerr << __LINE__ << " - B_CODE: 0 (bench OK)" << std::endl;
                log_messages.printf(MSG_DEBUG, "code %d\n", code);

                /** Read Power (p/s) */
                retval = std::fscanf(f,"%llu\n", &power);
                if (retval != 1)
                {
                    std::cerr << __LINE__ << " - ERROR: Failed to read host power." << std::endl;
                    plan_new_benchamrk(host_id);
                    break;
                }

                /** In case of mask attack, convert power to mask indices/s */
                std::snprintf(buf, SQL_BUF_SIZE, "SELECT attack_mode FROM `%s` WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_job.c_str(), job_id);
                uint64_t attack_mode = get_num_from_mysql(buf);

                /** Save original power for fc_benchmark */
                long long unsigned int original_power = power;

                std::snprintf(buf, SQL_BUF_SIZE, "SELECT COUNT(*) FROM `%s` WHERE job_id = %" PRIu64 " LIMIT 1;", mysql_table_hash.c_str(), job_id);
                uint64_t hashcount = get_num_from_mysql(buf);
                std::cerr<<__LINE__<<" - There are "<<hashcount<<" hashes, dividing power\n";
                //benchmark sends hashes/second, so if there are more we need to divide by that to get keys/second
                power /= hashcount;

                if (attack_mode == 3)
                {
                    std::snprintf(buf, SQL_BUF_SIZE, "SELECT keyspace FROM `%s` WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_job.c_str(), job_id);
                    uint64_t keyspace = get_num_from_mysql(buf);

                    std::snprintf(buf, SQL_BUF_SIZE, "SELECT hc_keyspace FROM `%s` WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_job.c_str(), job_id);
                    uint64_t hc_keyspace = get_num_from_mysql(buf);

                    /** Job with keyspace == 0 is malformed */
                    if (hc_keyspace == 0 || keyspace == 0)
                    {
                        std::cerr << __LINE__ << " - Keyspace cannot be 0, setting job to Malformed status" << std::endl;
                        std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET status = %d WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_job.c_str(), Job_malformed, job_id);
                        update_mysql(buf);
                        break;
                    }

                    uint64_t factor = keyspace / hc_keyspace;

                    if (factor > 1)
                    {
                        power /= factor * FIRST_WU_SHRINK_FACTOR;
                        std::cerr << __LINE__ << " - Updating power for mask-attack with ratio: 1/" << factor << ", new power: " << power << std::endl;
                    }
                }

                //power is in hashes per second, and rules multiply the keyspace
                std::snprintf(buf, SQL_BUF_SIZE, "SELECT rules FROM `%s` WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_job.c_str(), job_id);
                std::string rulesFilename = get_str_from_mysql(buf);
                if(!rulesFilename.empty())
                {
                    std::ifstream rulesFile(Config::rulesDir+rulesFilename);
                    if(rulesFile)
                    {
                        size_t ruleCount = 0;
                        std::string line;
                        while(std::getline(rulesFile, line))
                        {
                            if(!line.empty())
                            {
                                ruleCount += 1;
                            }
                        }
                        power /= ruleCount;
                    }
                }

                /** Update fc_benchmark power */
                std::snprintf(buf, SQL_BUF_SIZE, "SELECT hash_type FROM `%s` WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_job.c_str(), job_id);
                uint64_t hash_type = get_num_from_mysql(buf);
                std::map<uint32_t, uint64_t> speed_map;

                if (find_benchmark_results(speed_map, boinc_host_id) && speed_map.find(hash_type) != speed_map.end())
                {
                    /** Entry already exist, we must update it */
                    std::cerr << __LINE__ << " - Updating entry in fc_benchmark: " << hash_type << ":" << original_power << std::endl;
                    std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET `power` = %llu, `last_update` = now() WHERE `boinc_host_id` = %" PRIu64 " AND `hash_type` = %" PRIu64 " LIMIT 1 ;",
                        mysql_table_benchmark.c_str(), original_power, boinc_host_id, hash_type);
                    update_mysql(buf);
                }
                else
                {
                    /** Entry does not exist, create it */
                    std::cerr << __LINE__ << " - Adding new entry to fc_benchmark: " << hash_type << ":" << original_power << std::endl;
                    std::snprintf(buf, SQL_BUF_SIZE, "INSERT INTO `%s` (`boinc_host_id`,`hash_type`,`power`) VALUES (%" PRIu64 ", %" PRIu64 ", %llu) ;",
                        mysql_table_benchmark.c_str(), boinc_host_id, hash_type, original_power);
                    update_mysql(buf);
                }

                /** Update fc_host power */
                std::cerr << __LINE__ << " - New host power: " << power << std::endl;
                std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET power = %llu, status = %d, time = now() WHERE id = %" PRIu64 " ;", mysql_table_host.c_str(), power ? power : 1, Host_normal, host_id);
                update_mysql(buf);

                /** Read cracking_time */
                retval = std::fscanf(f,"%lf\n", &cracking_time);
                if (retval != 1)
                {
                    std::cerr << __LINE__ << " - ERROR: Failed to read cracking_time." << std::endl;
                    plan_new_benchamrk(host_id);
                    break;
                }
                std::cerr << __LINE__ << " - Benchmark time: " << cracking_time << "s" << std::endl;

                std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET cracking_time = cracking_time + %lf WHERE id = %" PRIu64 " ;", mysql_table_job.c_str(), cracking_time, job_id);
                update_mysql(buf);

                std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET cracking_time = %lf WHERE `workunit_id` = %lu LIMIT 1;", mysql_table_workunit.c_str(), cracking_time, wu.id);
                update_mysql(buf);
            }

            /** Result ERROR */
            else
            {
                std::cerr << __LINE__ << " - B_CODE: " << code <<" (bench ERROR)" << std::endl;
                log_messages.printf(MSG_DEBUG, "code %d\n", code);

                plan_new_benchamrk(host_id);
            }
        }

        /** NORMAL mode */
        else if(mode == 'n')
        {
            std::cerr << __LINE__ << " - WU_MODE: NORMAL (n)" << std::endl;
            log_messages.printf(MSG_DEBUG, "mode n\n");

            /** Read the code */
            retval = std::fscanf(f,"%d\n", &code);
            if (retval != 1)
            {
                std::cerr << __LINE__ << " - ERROR: Failed to read status code." << std::endl;
                std::fclose(f);
                set_retry_workunit(wu.id);
                return 0;
            }

            /** Read workunit keyspace for updating*/
            std::snprintf(buf, SQL_BUF_SIZE, "SELECT hc_keyspace FROM `%s` WHERE workunit_id = %lu LIMIT 1;", mysql_table_workunit.c_str(), wu.id);
            uint64_t hc_keyspace = get_num_from_mysql(buf);
            uint64_t power_keyspace = hc_keyspace;

            /** In case of rules attack, multiply the keyspace by number of rules */
            std::snprintf(buf, SQL_BUF_SIZE, "SELECT attack_mode FROM `%s` WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_job.c_str(), job_id);
            uint64_t attack_mode = get_num_from_mysql(buf);

            /** Read workunit properties */
            std::snprintf(buf, SQL_BUF_SIZE, "SELECT seconds_per_workunit FROM `%s` WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_job.c_str(), job_id);
            uint64_t seconds_per_workunit = get_num_from_mysql(buf);

            std::snprintf(buf, SQL_BUF_SIZE, "SELECT power FROM `%s` WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_host.c_str(), host_id);
            uint64_t old_power = get_num_from_mysql(buf);

            /** SOME of the passwords FOUND */
            if (code == 0)
            {
                std::cerr << __LINE__ << " - N_CODE: 0 (some of passes FOUND)" << std::endl;
                log_messages.printf(MSG_DEBUG, "code %d\n", code);

                /** Read cracking_time */
                retval = std::fscanf(f,"%lf\n", &cracking_time);
                if (retval == 1)
                {
                    std::cerr << __LINE__ << " - Cracking time: " << cracking_time << "s" << std::endl;

                    std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET cracking_time = cracking_time + %lf WHERE id = %" PRIu64 " ;", mysql_table_job.c_str(), cracking_time, job_id);
                    update_mysql(buf);

                    std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET cracking_time = %lf WHERE `workunit_id` = %lu LIMIT 1;", mysql_table_workunit.c_str(), cracking_time, wu.id);
                    update_mysql(buf);
                }

                /** Parse the results */
                if (is_binary(job_id))
                {
                    /** Read the whole file */
                    if (fread(hash_string, 1, MAX_HASH_SIZE, f) == 0)
                        std::cerr << __LINE__ << " - ERROR: Failed to read binary hash" << std::endl;
                    else if (!feof(f))
                        std::cerr << __LINE__ << " - ERROR: Binary hash too large" << std::endl;
                    else
                    {
                        char * found_hash = std::strrchr(hash_string, ':');
                        if (!found_hash)
                            std::cerr << __LINE__ << " - ERROR: Failed to parse binary hash-line" << std::endl;
                        else
                        {
                            /** Replace : with 0, effectively splitting the array*/
                            found_hash[0] = 0;
                            ++found_hash;

                            std::cerr << __LINE__ << " - Hash: *binary*" << std::endl;
                            std::cerr << __LINE__ << " - Cracked password: " << found_hash << std::endl;

                            /** Save the password to DB */
                            std::snprintf(buf, SQL_BUF_SIZE,
                                          "UPDATE `%s` SET `result` = '%s', `time_cracked` = NOW() WHERE `job_id` = %" PRIu64 " AND `result` IS NULL LIMIT 1; ",
                                          mysql_table_hash.c_str(), found_hash, job_id);
                            update_mysql(buf);
                        }
                    }
                }
                else
                {
                    /** Read the results line by line */
                    while (std::fgets(hash_string, MAX_HASH_SIZE, f))
                    {
                        /** Remove trailing \n nad \r */
                        char *pos;
                        if ((pos = strchr(hash_string, '\r')))
                            *pos = '\0';
                        if ((pos = strchr(hash_string, '\n')))
                            *pos = '\0';

                        /** Find start of found pass */
                        char * found_hash = std::strrchr(hash_string, ':');

                        if (!found_hash)
                        {
                            std::cerr << __LINE__ << " - ERROR: Failed to parse hash-line" << std::endl;
                            break;
                        }

                        /** Replace : with 0, effectively splitting the array*/
                        found_hash[0] = 0;
                        ++found_hash;

                        std::cerr << __LINE__ << " - Hash: " << hash_string << std::endl;
                        std::cerr << __LINE__ << " - Cracked password: " << found_hash << std::endl;

                        /** Save the password to DB */
                        std::snprintf(buf, SQL_BUF_SIZE,
                                      "UPDATE `%s` SET `result` = '%s', `time_cracked` = NOW() WHERE `job_id` = %" PRIu64 " AND LOWER(CONVERT(`hash` USING latin1)) = LOWER(CONVERT('%s' USING latin1)) AND `result` IS NULL ; ",
                                      mysql_table_hash.c_str(), found_hash, job_id, hash_string);
                        update_mysql(buf);
                    }
                }

                /** Check if the job has any hashes left */
                if (no_hashes_left(job_id))
                {
                    /** Finish the job */
                    std::snprintf(buf, SQL_BUF_SIZE, "CALL finish_job(%" PRIu64 ", %d);", job_id, Job_finished);
                    update_mysql(buf);

                    std::cerr << __LINE__ << " - Canceling all workunits for job_id " << job_id << std::endl;
                    workunits = find_workunits2(job_id, "");
                    cancel_workunits2(workunits);

                    if (deleteFlag)
                        delete_workunits2(workunits);
                    else
                        finish_workunits(workunits);

                    /** Kill PCFG Manager */
                    if (attack_mode == 9)
                    {
                        PretermClient m_client(job_id);
                        m_client.Kill();
                        std::cerr << __LINE__ << " - PCFG Manager killed " << job_id << std::endl;
                    }
                }
                else
                {
                    // Update job progress
                    std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET indexes_verified = indexes_verified + %" PRIu64 " WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_job.c_str(), hc_keyspace, job_id);
                    update_mysql(buf);

                    /** Update the host power, if workunit was large enough (more then 1/2 benchmarked power) */
                    if (power_keyspace > (0.5 * seconds_per_workunit * old_power))
                        update_power(host_id, power_keyspace, cracking_time);
                    else
                        std::cerr << __LINE__ << " - WARNING: Assimilated workunit is too small for update_power calculation" << std::endl;
                }
            }

            /** Password NOT yet found */
            else if(code == 1)
            {
                std::cerr << __LINE__ << " - N_CODE: 1 (pass NOT found)" << std::endl;
                log_messages.printf(MSG_DEBUG, "code %d\n", code);



                /** Read cracking_time */
                retval = std::fscanf(f,"%lf\n", &cracking_time);
                if (retval == 1)
                {
                    std::cerr << __LINE__ << " - Cracking time: " << cracking_time << "s" << std::endl;

                    std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET cracking_time = cracking_time + %lf WHERE id = %" PRIu64 " ;", mysql_table_job.c_str(), cracking_time, job_id);
                    update_mysql(buf);

                    std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET cracking_time = %lf WHERE `workunit_id` = %lu LIMIT 1;", mysql_table_workunit.c_str(), cracking_time, wu.id);
                    update_mysql(buf);

                    /** Update the host power, if workunit was large enough (more then 1/2 benchmarked power) */
                    if (power_keyspace > (0.15 * seconds_per_workunit * old_power))
                        update_power(host_id, power_keyspace, cracking_time);
                    else
                        std::cerr << __LINE__ << " - WARNING: Assimilated workunit is too small for update_power calculation" << std::endl;
                }
                else
                    std::cerr << __LINE__ << " - WARNING: Was NOT able to read cracking_time of a workunit, not updating power" << std::endl;

                // Update job progress
                std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET indexes_verified = indexes_verified + %" PRIu64 " WHERE id = %" PRIu64 " LIMIT 1;", mysql_table_job.c_str(), hc_keyspace, job_id);
                update_mysql(buf);

            }

            /** Host error */
            else
            {
                /** Cancel and delete host workunits and put them to Retry */
                log_messages.printf(MSG_DEBUG, "code %d\n", code);
                std::cerr << __LINE__ << " - N_CODE: " << code << " (host ERROR)" << std::endl;

                std::cerr << __LINE__ << " - Updating power to 0 and status to Benchmark for host_id " << host_id << std::endl;
                std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET power = 0, status = %d WHERE id = %" PRIu64 " ;", mysql_table_host.c_str(), Host_benchmark, host_id);
                update_mysql(buf);

                std::snprintf(buf, SQL_BUF_SIZE, "`host_id`='%" PRIu64 "'", host_id);

                std::cerr << __LINE__ << " - Canceling all workunits for host_id " << host_id << " with job_id" << job_id << std::endl;
                workunits = find_workunits2(job_id, buf);
                cancel_workunits2(workunits);

                std::cerr << __LINE__ << " - Adding them to Retry and deleting them from Workunits" << std::endl;
                std::vector<MysqlWorkunit *>::iterator workunitIt;
                for(workunitIt = workunits.begin(); workunitIt != workunits.end(); ++workunitIt)
                    set_retry_workunit((*workunitIt)->m_workunit_id);

                // Do not set pacakge to finished/delete
                std::fclose(f);
                return 0;
            }
        }

        /** BENCH_ALL mode */
        else if(mode == 'a')
        {
            std::cerr << __LINE__ << " - WU_MODE: BENCH_ALL (a)" << std::endl;
            log_messages.printf(MSG_DEBUG, "mode a\n");

            /** Read the code */
            retval = std::fscanf(f,"%d\n", &code);
            if (retval != 1)
            {
                std::cerr << __LINE__ << " - ERROR: Failed to read status code." << std::endl;
                plan_new_benchamrk(host_id);
                break;
            }

            /** Bench successful */
            if(code == 0)
            {
                std::cerr << __LINE__ << " - A_CODE: 0 (bench DONE)" << std::endl;
                log_messages.printf(MSG_DEBUG, "code %d\n", code);

                /** Read cracking_time */
                retval = std::fscanf(f,"%lf\n", &cracking_time);
                if (retval != 1)
                {
                    std::cerr << __LINE__ << " - ERROR: Failed to read cracking_time." << std::endl;
                    plan_new_benchamrk(host_id);
                    break;
                }

                std::cerr << __LINE__ << " - Benchmark time: " << cracking_time << "s" << std::endl;

                std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET cracking_time = %lf WHERE `workunit_id` = %lu LIMIT 1 ;",
                    mysql_table_workunit.c_str(), cracking_time, wu.id);
                update_mysql(buf);

                /** Read the results */
                std::map<uint32_t, uint64_t> speed_map; /** Use map as fastest STL container for find */
                uint32_t hash_type;

                if (find_benchmark_results(speed_map, boinc_host_id))
                {
                    /** Benchamrk was already run in the past */
                    while (std::fscanf(f,"%u:%llu\n", &hash_type, &power) == 2)
                    {
                        auto it = speed_map.find(hash_type);
                        if (it != speed_map.end())
                        {
                            /** Entry already exist, we must update it */
                            std::cerr << __LINE__ << " - Updating entry in fc_benchmark: " << hash_type << ":" << power << std::endl;
                            std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET `power` = %llu, `last_update` = now() WHERE `boinc_host_id` = %" PRIu64 " AND `hash_type` = %u LIMIT 1 ;",
                                mysql_table_benchmark.c_str(), power, boinc_host_id, hash_type);
                            update_mysql(buf);

                            /** Remove updated entry for near-constant search time */
                            speed_map.erase(it);
                        }
                        else
                        {
                            /** Entry does not exist, create it */
                            std::cerr << __LINE__ << " - Adding new entry to fc_benchmark: " << hash_type << ":" << power << std::endl;
                            std::snprintf(buf, SQL_BUF_SIZE, "INSERT INTO `%s` (`boinc_host_id`,`hash_type`,`power`) VALUES (%" PRIu64 ", %u, %llu) ;",
                                mysql_table_benchmark.c_str(), boinc_host_id, hash_type, power);
                            update_mysql(buf);
                        }
                    }
                }
                else
                {
                    /** No results for this host yet */
                    while (std::fscanf(f,"%u:%llu\n", &hash_type, &power) == 2)
                    {
                        std::cerr << __LINE__ << " - Adding new entry to fc_benchmark: " << hash_type << ":" << power << std::endl;
                        std::snprintf(buf, SQL_BUF_SIZE, "INSERT INTO `%s` (`boinc_host_id`,`hash_type`,`power`) VALUES (%" PRIu64 ", %u, %llu) ;",
                            mysql_table_benchmark.c_str(), boinc_host_id, hash_type, power);
                        update_mysql(buf);
                    }
                }

                /** Set host to status DONE */
                std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET status = %d WHERE id = %" PRIu64 " ;", mysql_table_host.c_str(), Host_done, host_id);
                update_mysql(buf);

                /** Set job to finished when all host are done */
                std::snprintf(buf, SQL_BUF_SIZE, "SELECT COUNT(*) FROM `%s` WHERE job_id = %d AND status = %d ;", mysql_table_host.c_str(), BENCHALL_JOB_ID, Host_benchmark);
                unsigned int row_cnt = get_num_from_mysql(buf);

                if (row_cnt == 0)
                {
                    std::cerr << __LINE__ << " - BENCH_ALL finished, setting pacakge status to finished" << std::endl;
                    std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET status = %d, time_end = now() WHERE id = %d LIMIT 1 ;", mysql_table_job.c_str(), Job_finished, BENCHALL_JOB_ID);
                    update_mysql(buf);
                }
            }
            /** hashcat error occured */
            else
            {
                std::cerr << __LINE__ << " - A_CODE: " << code << " (bench ERROR)\nHost cannot run complete benchmark." << std::endl;

                /** Find out what happened */
                retval = std::fscanf(f,"%255s\n", param);
                if (retval == 1)
                    std::cerr << __LINE__ << " - ERROR: " << param << std::endl;
            }
        }

        if (deleteFlag)
        {
            /** Delete finished workunit */
            std::cerr << __LINE__ << " - Deleting assimilated workunit with workId " << wu.id << std::endl;
            std::snprintf(buf, SQL_BUF_SIZE, "DELETE FROM `%s` WHERE workunit_id = %lu ;", mysql_table_workunit.c_str(), wu.id);
            update_mysql(buf);
        }
        else
        {
            /** Set workunit finished flag */
            std::cerr << __LINE__ << " - Setting finished flag to assimilated workunit with workunit_id " << wu.id << std::endl;
            std::snprintf(buf, SQL_BUF_SIZE, "UPDATE `%s` SET finished = 1, progress = 100 WHERE workunit_id = %lu ;", mysql_table_workunit.c_str(), wu.id);
            update_mysql(buf);
        }

        std::fclose(f);
    }

    return 0;
}
