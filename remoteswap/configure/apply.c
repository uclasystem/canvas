#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/syscall.h>

#define MAX_PROC_NUM 50
#define MAX_CORES_NUM 200
#define MAX_PROC_NAME_LENGTH 100
#define MAX_SCHEDULER_NUM 4

struct proc_info {
	int num_apps;
	int proc_name_length;
} info;

struct threshold_info {
	int scheduler_threshold;
	int auto_maintain_time;
} tinfo;

/*
 * ProcInfo
 * AppsNum #Less than 50
 *          2
 * MaxNameLength #Less than 200
 *          20
 * ProcName
 *          Memcached, Spark
 * ProcThreadsNum
 *          4, 24
 * ProcCoresAllocation #Follow the order above
 *          2, 4, 66, 68
 *          1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 65, 67, 69, 71, 73, 75, 77, 79, 72, 74, 76, 78
 * ProcWeights
 *          1, 1

 * SchedulerInfo #SchedulerNum 4
 * SchedulerCores
 *          6, 8, 10, 12
 * SchedulerThreshold
 *          25000
 * SchedulerPolicyBoundary
 *          0, 10, 20, 30
*/

char idname[MAX_PROC_NUM];
char proc_name[MAX_PROC_NUM][MAX_PROC_NAME_LENGTH];
char names[MAX_PROC_NUM * MAX_PROC_NAME_LENGTH];
int apps_num = 0;
int max_name_length = 0;
int proc_threads_num[MAX_PROC_NUM];
int proc_cores[MAX_PROC_NUM][MAX_CORES_NUM];
int cores[MAX_PROC_NUM * MAX_CORES_NUM];
int proc_weights[MAX_PROC_NUM];
int proc_lat[MAX_PROC_NUM];

int scheduler_cores[MAX_SCHEDULER_NUM];
int scheduler_threshold;
int scheduler_check_duration;
int scheduler_poll_times;
int auto_maintain_time;
int scheduler_policy_boundary[MAX_SCHEDULER_NUM];

void ReadOthers(FILE *fp)
{
	while (fgetc(fp) != '\n')
		;
}

void D2toD1(void)
{
	int i, j;
	int total_threads_num = 0;
	for (i = 0; i < apps_num; i++) {
		for (j = 0; j < max_name_length; j++)
			names[i * max_name_length + j] = proc_name[i][j];
	}
	for (i = 0; i < apps_num; i++) {
		for (j = 0; j < proc_threads_num[i]; j++)
			cores[total_threads_num + j] = proc_cores[i][j];
		total_threads_num += proc_threads_num[i];
	}
}

int main(int argc, char *argv[])
{
	int i = 0;

	printf("We suggest you stopping all the running applications,\n");
	printf("Please make sure that ProcConfig.txt and SchedulerConfig.txt exist...\n\n");

	FILE *pfp = fopen("ProcConfig.txt", "r+");
	FILE *sfp = fopen("SchedulerConfig.txt", "r+");

	if (pfp == NULL) {
		printf("Error: ProcConfig.txt does not exist!\n");
		goto end;
	}
	if (sfp == NULL) {
		printf("Error: SchedulerConfig.txt does not exist!\n");
		goto end;
	}

	fscanf(pfp, "%s", idname);
	if (strcmp(idname, "ProcInfo") != 0) {
		printf("The headline of ProcConfig file is %s, instead of ProcInfo\n", idname);
		goto next;
	}
	while (fscanf(pfp, "%s", idname) != EOF) {
		if (strcmp(idname, "AppsNum") == 0) {
			ReadOthers(pfp);
			fscanf(pfp, "%d", &apps_num);
			printf("Apps num: %d\n", apps_num);
			if (apps_num > MAX_PROC_NUM) {
				printf("Error! The number of procs should not exceed 50.\n");
				goto next;
			}
		} else if (strcmp(idname, "MaxNameLength") == 0) {
			ReadOthers(pfp);
			fscanf(pfp, "%d", &max_name_length);
			printf("Max name length: %d\n", max_name_length);
			if (max_name_length > MAX_PROC_NAME_LENGTH) {
				printf("Error! The max length of proc name should not exceed 100.\n");
				goto next;
			}
		} else if (strcmp(idname, "ProcName") == 0) {
			ReadOthers(pfp);
			for (i = 0; i < apps_num; i++) {
				fscanf(pfp, "%s", proc_name[i]);
				printf("Proc name[%d]: %s\n", i, proc_name[i]);
			}
		} else if (strcmp(idname, "ProcThreadsNum") == 0) {
			ReadOthers(pfp);
			for (i = 0; i < apps_num; i++) {
				fscanf(pfp, "%d", &proc_threads_num[i]);
				printf("Proc threads num[%d]: %d\n", i, proc_threads_num[i]);
				if (proc_threads_num[i] > MAX_CORES_NUM) {
					printf("Error! The max number of threads in a proc should not exceed 200.\n");
					goto next;
				}
			}
		} else if (strcmp(idname, "ProcCoresAllocation") == 0) {
			ReadOthers(pfp);
			for (i = 0; i < apps_num; i++) {
				int j;
				printf("Proc cores[%d]:", i);
				for (j = 0; j < proc_threads_num[i]; j++) {
					fscanf(pfp, "%d", &proc_cores[i][j]);
					printf(" %d", proc_cores[i][j]);
				}
				printf("\n");
			}
		} else if (strcmp(idname, "ProcWeights") == 0) {
			ReadOthers(pfp);
			for (i = 0; i < apps_num; i++) {
				fscanf(pfp, "%d", &proc_weights[i]);
				printf("Proc weight[%d]: %d\n", i, proc_weights[i]);
			}
		} else if (strcmp(idname, "ProcLatencyCritical") == 0) {
			ReadOthers(pfp);
			for (i = 0; i < apps_num; i++) {
				fscanf(pfp, "%d", &proc_lat[i]);
				printf("Proc Lat-Critical[%d]: %d\n", i, proc_lat[i]);
			}
		}
	}
next:
	fclose(pfp);

	fscanf(sfp, "%s", idname);
	if (strcmp(idname, "SchedulerInfo") != 0) {
		printf("The headline of SchedulerConfig file is %s, instead of SchedulerInfo\n", idname);
		goto end;
	}
	while (fscanf(sfp, "%s", idname) != EOF) {
		if (strcmp(idname, "SchedulerCores") == 0) {
			ReadOthers(sfp);
			for (i = 0; i < MAX_SCHEDULER_NUM; i++) {
				fscanf(sfp, "%d", &scheduler_cores[i]);
				printf("Scheduler cores: %d\n", scheduler_cores[i]);
			}
		} else if (strcmp(idname, "SchedulerThreshold") == 0) {
			ReadOthers(sfp);
			fscanf(sfp, "%d", &scheduler_threshold);
			if (scheduler_threshold < 0) {
				printf("Scheduler threshold: auto, with upper bound: %dMB/s, or %d us\n",
				       -scheduler_threshold, -scheduler_threshold);
			} else {
				printf("Scheduler threshold: %dMB/s, or %d us\n", scheduler_threshold,
				       scheduler_threshold);
			}
		} else if (strcmp(idname, "SchedulerAutoMaintainTime") == 0) {
			ReadOthers(sfp);
			fscanf(sfp, "%d", &auto_maintain_time);
			printf("Scheduler auto matain time: %ds\n", auto_maintain_time);
		} else if (strcmp(idname, "SchedulerPolicyBoundary") == 0) {
			ReadOthers(sfp);
			for (i = 0; i < MAX_SCHEDULER_NUM; i++) {
				fscanf(sfp, "%d", &scheduler_policy_boundary[i]);
				if (i == 0 && scheduler_policy_boundary[i] < 0) {
					printf("Scheduler policy boundary: auto, initial configuration fllows\n");
					printf("Scheduler policy boundary: %d\n", 0);
				} else
					printf("Scheduler policy boundary: %d\n", scheduler_policy_boundary[i]);
			}
		} else if (strcmp(idname, "SchedulerCheckDuration") == 0) {
			ReadOthers(sfp);
			fscanf(sfp, "%d", &scheduler_check_duration);
			printf("Scheduler check duration: %d\n", scheduler_check_duration);
		} else if (strcmp(idname, "SchedulerPollTimes") == 0) {
			ReadOthers(sfp);
			fscanf(sfp, "%d", &scheduler_poll_times);
			printf("Scheduler poll times: %d\n", scheduler_poll_times);
		}
	}
	fclose(sfp);

	info.num_apps = apps_num;
	info.proc_name_length = max_name_length;

	tinfo.scheduler_threshold = scheduler_threshold;
	tinfo.auto_maintain_time = auto_maintain_time;

	D2toD1();

	printf("\n");
	do {
		printf("If this is your first time to apply configuration after insmod\n");
		printf("It will start the scheduler\n");
		printf("Enter Confirm to continue, or Quit to cancel...\n");
		scanf("%s", idname);
		if (strcmp(idname, "Quit") == 0) {
			printf("Quit successfully\n");
			goto end;
		}
	} while (strcmp(idname, "Confirm") != 0);

	syscall(462, scheduler_cores, &tinfo, scheduler_policy_boundary, scheduler_check_duration,
		scheduler_poll_times);
	syscall(463, &info, names, cores, proc_threads_num, proc_weights, proc_lat);

	printf("Set Done!\n");
end:
	return 0;
}
