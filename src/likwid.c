#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>

#include <likwid.h>

#include "config.h" // sets several defines

// headers required for collectd
#include "collectd.h"
#include "common.h" /* auxiliary functions */
#include "plugin.h" /* plugin_register_*, plugin_dispatch_values */

#define PLUGIN_NAME "likwid"

static int  accessMode = 0;
static int  mTime = 15;       /**< Measurement time per group in seconds */
static cdtime_t mcdTime = 0;// TIME_T_TO_CDTIME_T(15);
static int  startSecond = 20;

static int likwid_verbose = 1;

static bool summarizeFlops = true;
static int sumFlopsGroupId = -1;
static int sumFlopsMetricId = -1;
// TODO: different handling, if no or only one FLOP metric is measured

static int numCPUs = 0;
static int* cpus = NULL;

static int numSockets = 0;
static int* socketInfoCores = NULL;

/*! \brief Metric type */
typedef struct metric_st {
  char* name;     /*!< metric name */
  uint8_t xFlops; /*!< if > 0, it is a FLOPS metric and the value is 
                       the multiplier for normalization */
  double *values; /*!< values for each CPU or socket */
  bool percpu;    /*!< true, if values are per CPU, otherwise per socket is assumed */
}metric_t;

/*! \brief Metric group type */
typedef struct metric_group_st {
  int id;            /*!< group ID */
  char* name;        /*!< group name */
  int numMetrics;    /*!< number of metrics in this group */
  metric_t *metrics; /*!< metrics in this group */
}metric_group_t;

static int numGroups = 0;
static metric_group_t* metricGroups = NULL;

int numValues = 0;       /**< number of values for next dispatch */
value_t* values = NULL;  /**< values to dispatch */
char* valueNames = NULL; /**< Metric name of the dispatched values */

static char* mystrdup(const char *s)
{
  size_t len = strlen (s) + 1;
  char *result = (char*) malloc (len);
  if (result == (char*) 0)
    return (char*) 0;
  return (char*) memcpy (result, s, len);
}

/*! brief Determines by metric name, whether this is a per CPU or per socket metric */
static bool _isMetricPerCPU(const char* metric)
{
  if(0 == strncmp("mem_bw", metric, 6) || 0 == strncmp("rapl_power", metric, 6))
  {
    return false;
  }
  else
  {
    return true;
  }
}

void _setupGroups()
{
  if(NULL == metricGroups)
  {
    ERROR(PLUGIN_NAME "No metric groups allocated! Plugin not initialized?");
    return;
  }

  INFO(PLUGIN_NAME ": Setup metric groups");

  //int numFlopMetrics = 0;
  //TODO: set summarizeFlops = false; if less than two FLOP metrics are measured
  //      no values allocation needed
  
  // set the group IDs and metric names
  for(int g = 0; g < numGroups; g++)
  {
    if(metricGroups[g].name != NULL )
    {
      int gid = perfmon_addEventSet(metricGroups[g].name);
      if(gid < 0)
      {
        metricGroups[g].id = -2;
        INFO(PLUGIN_NAME ": Failed to add group %s to LIKWID perfmon module", metricGroups[g].name);
      }
      else
      {
        // set the group ID
        metricGroups[g].id = gid;

        // get number of metrics for this group
        int numMetrics = perfmon_getNumberOfMetrics(gid);
        metricGroups[g].numMetrics = numMetrics;
        if(numMetrics == 0)
        {
          WARNING(PLUGIN_NAME ": Group %s has no metrics!", metricGroups[g].name);
          continue;
        }

        // allocate metric array
        metric_t* metrics = (metric_t*) malloc(numMetrics * sizeof(metric_t));
        if(NULL == metrics)
        {
          metricGroups[g].numMetrics = 0;
          metricGroups[g].id = -2;
          WARNING(PLUGIN_NAME ": Disable group %s as memory for metrics could not be allocated", metricGroups[g].name);
          continue;
        }

        // set the pointer to the allocated memory for metrics
        metricGroups[g].metrics = metrics;

        // set metric names and set initial values to -1
        for(int m = 0; m < numMetrics; m++)
        {
          metrics[m].name = perfmon_getMetricName(gid, m);

          // determine if metric is per CPU or per socket (by name)
          metrics[m].percpu = _isMetricPerCPU(metrics[m].name);

          // allocate memory for each CPU (value)
          metrics[m].values = (double*)malloc(numCPUs * sizeof(double));
          if(NULL == metrics[m].values)
          {
            metricGroups[g].id = -2;
            WARNING(PLUGIN_NAME "Disable group %s as memory for metric cpu values could not be allocated", metricGroups[g].name);
            continue;
          }

          size_t mlen = strlen(metrics[m].name);

          // initialize all CPU values to -1.0
          for( int c = 0; c < numCPUs; c++ )
          {
            metrics[m].values[c] = -1.0;
          }

          // normalize flops, if enabled
          if( summarizeFlops && 0 == strncmp("flops", metrics[m].name, 5) )
          {
            // double precision to single precision = factor 2
            if(0 == strncmp("dp", metrics[m].name + 6, 5))
            {
              metrics[m].xFlops = 2;
            }
            // // avx to single precision = factor 4
            else if(0 == strncmp("avx", metrics[m].name + 6, 5))
            {
              metrics[m].xFlops = 4;
            }
            else // assume single precision otherwise
            {
              metrics[m].xFlops = 1;
            }
            
            /* use the first metric in the list that starts with "flops" to store 
               total normalized flops */
            if(sumFlopsMetricId == -1)
            {
              sumFlopsMetricId = m;
              sumFlopsGroupId = g;
            }
          }
          else
          {
            metrics[m].xFlops = 0;
          }
        } // END for metrics
      }
    }
    else
    {
      // set group ID to invalid
      metricGroups[g].id = -1;
    }
  } // END: for groups
}

static int _init_likwid(void)
{
  perfmon_setVerbosity(likwid_verbose);
  
  topology_init();
  numa_init();
  affinity_init();
  //timer_init();
  HPMmode(accessMode);
  
  double timer = 0.0;
  CpuInfo_t cpuinfo = get_cpuInfo();
  CpuTopology_t cputopo = get_cpuTopology();
  numCPUs = cputopo->activeHWThreads;
  cpus = malloc(numCPUs * sizeof(int));
  if(!cpus)
  {   
      affinity_finalize();
      numa_finalize();
      topology_finalize();
      return 1;
  }

  int c = 0;
  for(int i = 0; i < cputopo->numHWThreads; i++)
  {   
      if (cputopo->threadPool[i].inCpuSet)
      {   
          cpus[c] = cputopo->threadPool[i].apicId;
          c++;
      }
  }

  // get socket information
  numSockets = cputopo->numSockets;
  uint32_t coresPerSocket = cputopo->numCoresPerSocket;
  socketInfoCores = malloc(numSockets*sizeof(int));
  if(NULL == socketInfoCores)
  {
    ERROR(PLUGIN_NAME ": Memory for socket information could not be allocated!");
    return 1;
  }
  for(int s = 0; s < numSockets; s++)
  {
    socketInfoCores[s] = s*coresPerSocket;
    INFO(PLUGIN_NAME ": Collecting per socket metrics for core: %d", socketInfoCores[s]);
  }

  NumaTopology_t numa = get_numaTopology();
  AffinityDomains_t affi = get_affinityDomains();
  //timer = timer_getCpuClock();
  perfmon_init(numCPUs, cpus);

  return 0;
}

static void _resetCounters(void)
{
  INFO(PLUGIN_NAME ": Set counters configuration!");

  for(int g = 0; g < numGroups; g++)
  {
    if(metricGroups[g].id < 0)
    {
      return;
    }

    perfmon_setCountersConfig(metricGroups[g].id);
  }
}

static const char* _getMeasurementName(metric_t *metric)
{
  if(metric->percpu)
  {
    return "likwid_cpu";
  }
  else
  {
    return "likwid_socket";
  }
}

/*! brief: cpu_idx is the index in the CPU array */
static bool _isSocketInfoCore(int cpu_idx)
{
  for(int s = 0; s < numSockets; s++) 
  {
    if(cpu_idx == socketInfoCores[s])
    {
      return true;
    }
  }
  return false;
}

// host "/" plugin ["-" plugin instance] "/" type ["-" type instance]
// e.g. taurusi2001/likwid_socket-0/cpi
// plugin field stores the measurement name (likwid_cpu or likwid_socket)
// the plugin instance stores the CPU ID
// the type field stores the metric name
// the type instance stores ???
static int _submit_value(const char* measurement, const char* metric, int cpu, double value, cdtime_t time)
{
  value_list_t vl = VALUE_LIST_INIT;
  value_t v = {.gauge = value};
 
  vl.values = &v;
  vl.values_len = 1;
  
  vl.time = time;

  //const char* mname = getMeasurementName(metric);
  
  sstrncpy(vl.plugin, measurement, sizeof(vl.plugin));
  sstrncpy(vl.type, "likwid", sizeof(vl.type));
  sstrncpy(vl.type_instance, metric, sizeof(vl.type_instance));
  snprintf(vl.plugin_instance, sizeof(vl.plugin_instance), "%i", cpu);

  INFO(PLUGIN_NAME ": dispatch: %s:%s(%d)=%lf", measurement, metric, cpu, value);

  plugin_dispatch_values(&vl);
}

static int likwid_plugin_read(void) 
{
  cdtime_t time = cdtime() + mcdTime * numGroups;
  
  INFO (PLUGIN_NAME ": %s:%d (timestamp: %.3f)", __FUNCTION__, __LINE__, CDTIME_T_TO_DOUBLE(time));
  
  // read from likwid
  for(int g = 0; g < numGroups; g++)
  {
    int gid = metricGroups[g].id;
    if(gid < 0)
    {
      INFO(PLUGIN_NAME ": No eventset specified for group %s", metricGroups[g].name);
      continue;
    }

    if(0 != perfmon_setupCounters(gid))
    {
      INFO(PLUGIN_NAME ": Could not setup counters for group %s", metricGroups[g].name);
      continue;
    }

    // measure counters for setup group
    perfmon_startCounters();
    sleep(mTime);
    perfmon_stopCounters();

    //int nmetrics = perfmon_getNumberOfMetrics(gid);
    int nmetrics = metricGroups[g].numMetrics;
    INFO(PLUGIN_NAME ": Measured %d metrics for %d CPUs for group %s (%d sec)", nmetrics, numCPUs, metricGroups[g].name, mTime);

    // for all active hardware threads
    for(int c = 0; c < numCPUs; c++)
    {
      // for all metrics in the group
      for(int m = 0; m < nmetrics; m++)
      {
        double metricValue = perfmon_getLastMetric(gid, m, c);
        metric_t *metric = &(metricGroups[g].metrics[m]);

        //INFO(PLUGIN_NAME ": %lu - %s(%d):%lf", CDTIME_T_TO_TIME_T(time), metric->name, cpus[c], metricValue);

        //REMOVE: check that we write the value for the correct metric
        const char* metricName = perfmon_getMetricName(gid, m);
        if( 0 != strcmp(metricName, metric->name))
        {
          WARNING(PLUGIN_NAME ": Something went wrong!!!");
        }

        // skip cores that do not provide values for per socket metrics
        if(!metric->percpu && !_isSocketInfoCore(c))
        {
          continue;
        }

        // normalize FLOP values to single precision
        if(summarizeFlops)
        {
          // if it is a flop metric
          if(metric->xFlops > 0)
          {
            // normalize, if it is not single precision
            if(metric->xFlops > 1 && metricValue > 0)
            {
              metricValue *= metric->xFlops;
            }

            //INFO(PLUGIN_NAME " FLOPS value set/add: %lu - %s(%d):%lf", CDTIME_T_TO_TIME_T(time), metric->name, cpus[c], metricValue);

            // set or add FLOPS
            // summarize into first "flop" metric that occurs
            if(-1 == metricGroups[sumFlopsGroupId].metrics[sumFlopsMetricId].values[c])
            {
              metricGroups[sumFlopsGroupId].metrics[sumFlopsMetricId].values[c] = metricValue;
            }
            else
            {
              metricGroups[sumFlopsGroupId].metrics[sumFlopsMetricId].values[c] += metricValue;
            }
          }
          else // other metrics than FLOPS
          {
            metric->values[c] = metricValue;
          }
        }
        else  // submit each raw metric
        {
          _submit_value(_getMeasurementName(metric), metric->name, cpus[c], metricValue, time);
        }
      }
    }
  }

  if(summarizeFlops)
  {
    for(int g = 0; g < numGroups; g++)
    {
      // skip groups that are not configured or invalid
      if(metricGroups[g].id < 0)
      {
        continue;
      }

      // for all active hardware threads
      for(int c = 0; c < numCPUs; c++)
      {
        // for all metrics in the group
        for(int m = 0; m < metricGroups[g].numMetrics; m++)
        {
          metric_t *metric = &(metricGroups[g].metrics[m]);
          double value = metric->values[c];

          // skip uninitialized values
          if(value == -1)
          {
            continue;
          }

          char* metricName = metric->name;

          // submit only normalized FLOPS (skip flop metrics except for "the chosen one"
          if(metricGroups[g].metrics[m].xFlops > 0)
          {
            if(g == sumFlopsGroupId && m == sumFlopsMetricId)
            {
              metricName = "flops_any";
            }
            else
            {
              continue;
            }
          }

          _submit_value(_getMeasurementName(metric), metricName, cpus[c], value, time);

          // reset counter value
          metricGroups[g].metrics[m].values[c] = -1;
        }
      }
    }
  }

  return 0;
}

static int likwid_plugin_init(void)
{
  INFO(PLUGIN_NAME ": %s:%d", __FUNCTION__, __LINE__);

  // set the cdtime based on the measurement time per group
  mcdTime = TIME_T_TO_CDTIME_T(mTime);

  int ret = _init_likwid();
  
  _setupGroups();
  
  return ret;
}

static int likwid_plugin_flush(cdtime_t timeout, const char *identifier, user_data_t *usr )
{
  
}

/*! brief Resets the likwid group counters

Example notification on command line:
echo "PUTNOTIF severity=okay time=$(date +%s) message=resetLikwidCounters" |   socat - UNIX-CLIENT:$HOME/sw/collectd/collectd-unixsock
 */
static int likwid_plugin_notify(const notification_t *type, user_data_t *usr )
{
  _resetCounters();
}

static int likwid_plugin_finalize( void )
{
  INFO (PLUGIN_NAME ": %s:%d", __FUNCTION__, __LINE__);

  //perfmon_finalize(); // segfault
  affinity_finalize();
  numa_finalize();
  topology_finalize();

  // free memory where CPU IDs are stored
  INFO(PLUGIN_NAME ": free allocated memory");
  if(NULL != cpus)
  {
    free(cpus);
  }

  if(NULL != metricGroups)
  {
    for(int i = 0; i < numGroups; i++)
    {
      // memory for group names have been allocated with strdup
      if(NULL != metricGroups[i].name)
      {
        free(metricGroups[i].name);
      }
      
      for(int m = 0; m < metricGroups[i].numMetrics; m++)
      {
        if(NULL != metricGroups[i].metrics[m].values)
        {
          free(metricGroups[i].metrics[m].values);
        }
      }
    }
    free(metricGroups);
  }
  //INFO(PLUGIN_NAME ": freed allocated memory");

  return 0;
}

static const char *config_keys[] =
{
  "SummarizeFlops",
  "AccessMode",
  "mtime",
  "groups",
  "StartSecond",
  "verbose"
};
static int config_keys_num = STATIC_ARRAY_SIZE(config_keys);

static int likwid_plugin_config (const char *key, const char *value)
{
  INFO (PLUGIN_NAME " config: %s := %s", key, value);
  
  if (strcasecmp(key, "SummarizeFlops") == 0)
  {
    summarizeFlops = IS_TRUE(value);
  }
  
  if (strcasecmp(key, "AccessMode") == 0)
  {
    accessMode = atoi(value);
  }
  
  if (strcasecmp(key, "mtime") == 0)
  {
    mTime = atoi(value);
    //mcdTime = TIME_T_TO_CDTIME_T(mTime);
  }
  
  if (strcasecmp(key, "verbose") == 0)
  {
    likwid_verbose = atoi(value);
  }
  
  if (strcasecmp(key, "groups") == 0)
  {
    // count number of groups
    numGroups = 1;
    int i = 0;
    while (value[i] != '\0') 
    { 
      if (value[i] == ' ') 
      {
        numGroups++;
      }
      i++;
    }
    
    // allocate metric group array
    metricGroups = (metric_group_t*)malloc(numGroups * sizeof(metric_group_t));
    if(NULL == metricGroups)
    {
      ERROR(PLUGIN_NAME " Could not allocate memory for metric groups: %s", value);
      return 1; // config failed
    }

    // inialize metric groups
    for(int i = 0; i < numGroups; i++)
    {
      metricGroups[i].id = -1;
      metricGroups[i].name = NULL;
      metricGroups[i].numMetrics = 0;
      metricGroups[i].metrics = NULL;
    }
    
    i = 0;
    char *grp_ptr;
    grp_ptr = strtok((char*)value, ",");
    while( grp_ptr != NULL )
    {
      // save group name
      metricGroups[i].name = mystrdup(grp_ptr);
      INFO(PLUGIN_NAME " Found group: %s", grp_ptr);
      
      // get next group
      grp_ptr = strtok(NULL, " ");
      
      i++;
    }
  }
  
  if (strcasecmp(key, "StartSecond") == 0)
  {
    startSecond = atoi(value);
  }
  
  return 0;
}

/*
 * This function is called after loading the plugin to register it with collectd.
 */
void module_register(void) {
  plugin_register_config (PLUGIN_NAME, likwid_plugin_config, config_keys, config_keys_num);
  plugin_register_read(PLUGIN_NAME, likwid_plugin_read);
  plugin_register_init(PLUGIN_NAME, likwid_plugin_init);
  plugin_register_shutdown(PLUGIN_NAME, likwid_plugin_finalize);
  plugin_register_flush(PLUGIN_NAME, likwid_plugin_flush, /* user data = */ NULL);
  plugin_register_notification(PLUGIN_NAME, likwid_plugin_notify, /* user data = */ NULL);
  return;
}
