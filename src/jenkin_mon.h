#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

typedef unsigned char u_int8;
typedef unsigned short u_int16;
typedef unsigned int   u_int32;
typedef unsigned long long u_int64;

typedef char int8;
typedef short int16;
typedef int   int32;
typedef long long int64;

typedef struct jobInfo
{
   char* jobPath;
   char* jobName;
   char statusInfoFile[40];
   char lastBuildInfoFile[40];
   struct jobInfo* p_nextJob;
}JobInfoT;

typedef enum color
{
   NO_BUILT,
   DISABLED,
   RED_COLOR,
   GRE_COLOR,
   BLU_COLOR,
   YEL_COLOR,
   CYA_COLOR,
   MAG_COLOR,
   WHI_COLOR,
   NON_COLOR
}ColorE;

typedef enum gpioStatus
{
   ON = 0,
   OF = 1
}GpioStatusE;

typedef struct color2LedInfo
{
   ColorE color;
   char* colorStr;
   GpioStatusE r;
   GpioStatusE g;
   GpioStatusE b;
}Color2LedInfoT;

Color2LedInfoT C2LInfo[] =
{
   {NO_BUILT , "notbuilt", OF, OF, OF},
   {DISABLED , "disabled", OF, OF, OF},
   {RED_COLOR, "red"     , ON, OF, OF},
   {GRE_COLOR, "green"   , OF, ON, OF},
   {BLU_COLOR, "blue"    , OF, OF, ON},
   {YEL_COLOR, "yellow"  , ON, ON, OF},
   {CYA_COLOR, "cyan"    , OF, ON, ON},
   {MAG_COLOR, "magenta" , ON, OF, ON},
   {WHI_COLOR, "white"   , ON, ON, ON},
   {NON_COLOR, "noColor" , OF, OF, OF}
};

typedef struct ledInfo
{
   ColorE color;
   bool isAnime;
}LedInfoT;

typedef struct groupInfo
{
   char* groupName;
   char* serverName;
   char* userName;
   char* passWord;
   u_int8 redLed;
   u_int8 greLed;
   u_int8 bluLed;
   LedInfoT ledStatus;
   bool isFirstDisplaySuccess;
   u_int16  displaySuccessTimeout;  // in second
   int64    lastSuccessTimeStamp;   // in second
   u_int32  lastBuildThreshold;     // in second 
   JobInfoT* p_allJobs;
   struct groupInfo* p_nextGroup;
}GroupInfoT;

typedef struct curlInfo
{
   char* curlServer;
   char* curlUser;
   char* curlPass;
   char curlCommand[20000];
   struct curlInfo* p_nextCurl;
	pthread_t curlThread;
}CurlInfoT;

GroupInfoT* getTailGroup(GroupInfoT* p_headGroup);

// Parse argument from command line
bool parseArgument(int argc, char* argv[]);

// Parse Group
bool parseXMLFile(const char* fileName, GroupInfoT** pp_headGroup);
bool parseGroupAttr(xmlDoc *doc, xmlNode *groupNode, GroupInfoT* p_group);
void printAllGroupInfo(GroupInfoT* p_headGroup);
void printGroupInfo(GroupInfoT* p_group);

// Parse Job 
bool parseJobsInfo(xmlDoc *doc, xmlNode *jobsNode, JobInfoT** p_headJob);
bool parseJobAttr(xmlDoc *doc, xmlNode *jobNode, JobInfoT* p_job);
void printAllJobInfo(JobInfoT* p_jobHead);
void printJobInfo(JobInfoT* p_job);

// Build Job Files
bool buildJobFiles(GroupInfoT* p_headGroup);

// Build Curl command
bool buildCurlCommand(GroupInfoT* p_headGroup, CurlInfoT** pp_headCurl);
CurlInfoT* getMatchCurl(char* server, char* user,
                        char* pass, CurlInfoT* p_headCulr);
void printAllCurlInfo(CurlInfoT* p_headCurl);

// Build threads for Curl
bool buildCurlThreads(CurlInfoT* p_headCurl);
void* curlThreadPoll(void *arg);
void waitAllCurlsThread(CurlInfoT* p_headCurl);
void cleanAllCurls(CurlInfoT* p_headCurl);

// Poll evaluate all color of all group
void* evalColorThreadPoll(void* arg);
void evalLedStatusForGroup(bool isAnime, bool isSuccess,
                           bool isThreshold, bool isAllDisable, GroupInfoT* p_group);
int64 timeStampFromFile(char* fileName);
int64 currentTimeStamp(void);
LedInfoT ledInfoFromfile(char* fileName);
void colorFromFile(char* fileName, char* colorStr, size_t strSize);
LedInfoT convert2LedInfo(char* colorStr);
char* convert2ColorString(LedInfoT led);

// Poll to display all color of all group
void* displayLedThreadPoll(void* arg);
void initAllGroupLed(GroupInfoT* p_headGroup);
void ledControl(LedInfoT led, u_int8 red, u_int8 green, u_int8 blue);
