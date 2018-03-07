
/*
 *   To compile:
 *    gcc -shared -o renice.so renice.c
 *
 */
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include <slurm/spank.h>
#include <slurm/slurm.h>
#include "src/common/xmalloc.h"

#define MAX_PATH_LEN 1024
#define BUFSIZE 128


/*
 * All spank plugins must define this macro for the SLURM plugin loader.
 */
SPANK_PLUGIN(dmtcp_spank, 2);

/*
 *  Minimum allowable value for priority. May be set globally
 *   via plugin option min_prio=<prio>
 */

static int dmtcp_disabled=0;
static const uint32_t default_dmtcp_port=7779; // DMTCP default port.
static const char cr_checkpoint_start[] = PKGLIBEXECDIR "/cr_start.sh";

static int _disable_dmtcp (int val, const char *optarg, int remote);

/*
 *  Provide a --with-dmtcp option to srun and sbatch:
 */
struct spank_option spank_options[] =
{
    { "no-dmtcp",NULL, "disable DMTCP checkpoint on the job being run", 0, 0,
        (spank_opt_cb_f) _disable_dmtcp
    },
    SPANK_OPTIONS_TABLE_END
};

/*
 *  Called from both srun and slurmd.
 */
int slurm_spank_init (spank_t sp, int ac, char **av){
   spank_option_register (sp, spank_options);
        return (ESPANK_SUCCESS);
}

int slurm_spank_task_init(spank_t sp, int ac, char **av){

  if (dmtcp_disabled != 0){
//    slurm_error("... no_dmtcp flag is ON, exit");
    spank_set_item(sp, S_JOB_CHECKPOINTABLE, 1);
    return (ESPANK_SUCCESS);
    }

 char srun_debug[10];
 int err = spank_getenv(sp, "SRUN_DEBUG", srun_debug, sizeof(srun_debug));
 if (spank_getenv(sp, "SRUN_DEBUG", srun_debug, sizeof(srun_debug)) ==0){
//    slurm_error("this is srun, exit");
    return (ESPANK_SUCCESS);
    }

//  slurm_error("This is sbatch, replace call");

   //we modify the application to be executed by including a DMTCP wrapper.
  char **argv;
  char **newArgv;
  uint32_t argc = 0;
  uint32_t cont = 0;

  err = spank_get_item (sp, S_JOB_ARGV, &argc,&argv);
  if (err != 0) {
  	slurm_error("error calling spank_get_item: %d", err);
	return (ESPANK_SUCCESS);
  }

  argc += 1;
  newArgv = xmalloc (sizeof(char*) * (argc + 1));
  newArgv[0] = strdup(cr_checkpoint_start);


  for (cont = 0; cont < argc-1; cont++)
    newArgv[cont+1] = strdup(argv[cont]);
   

  if (spank_set_item(sp, S_JOB_ARGV, &argc,&newArgv) != ESPANK_SUCCESS) {
    slurm_error("DMTCP Plugin could not be enabled");
    return (ESPANK_ERROR);
  }
  //slurm_error("DMTCP Plugin is enabled");

  return (ESPANK_SUCCESS);
}

int slurm_spank_task_exit(spank_t sp, int ac, char **av){
  //here we want to delete dmtcp_coordinator file and shutdown coordinator

  //job id, needed to access the rest of information
  uint32_t job_id;
  if (spank_get_item (sp, S_JOB_ID, &job_id) != 0){
    slurm_error("Could not get job id");
    return (ESPANK_ERROR);
  }

  char *ckpt_dir;
  if (spank_get_item (sp, S_CHECKPOINT_DIR, &ckpt_dir) != 0){
    slurm_error("Could not get checkpoint dir");
    return (ESPANK_ERROR);
  }

  char ckpt_file[MAX_PATH_LEN];
  sprintf(ckpt_file,"%s/%d/dmtcp_coordinator", ckpt_dir, job_id);
  remove (ckpt_file);

  return (ESPANK_SUCCESS);
 }

static int _disable_dmtcp (int val, const char *optarg, int remote)
{
    dmtcp_disabled=1;
    return (0);
}