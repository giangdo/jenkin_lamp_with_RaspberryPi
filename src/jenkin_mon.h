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
   struct jobInfo* p_nextJob;
   char* jobPath;
   char* jobName;
   char statusInfoFile[40];
   char lastBuildInfoFile[40];
}JobInfoT;

typedef enum color
{
   NO_BUILT,      // 0
   DISABLED,      // 1
   RED_COLOR,     // 2
   GRE_COLOR,     // 3
   BLU_COLOR,     // 4
   YEL_COLOR,     // 5
   CYA_COLOR,     // 6
   MAG_COLOR,     // 7
   WHI_COLOR,     // 8
   NON_COLOR      // 9
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

typedef struct groupStatus
{
   // TODO: should use bit field for this datatype.
   bool isAllDisable;
   bool isThreshold;
   bool isBuilding;
   bool isSuccess;
}GroupStatusT; 

typedef struct serverInfo
{
   char* serverName;
   char* userName;
   char* passWord;
}ServerInfoT;

typedef struct curlTimeInfo
{
   u_int8   maxTime;            // in second
   u_int8   pollTime;           // in second
}CurlTimeInfoT;

typedef struct groupInfo
{
   struct groupInfo* p_nextGroup;
   char* groupName;
   pthread_t evalColorThread;
   pthread_t ctrlLedThread;
   ServerInfoT server;
   u_int8 redLed;
   u_int8 greLed;
   u_int8 bluLed;
   LedInfoT ledStatus;
   GroupStatusT curSta;
   GroupStatusT preSta;
   bool isFirstDisplaySuccess;
   u_int16  displaySuccessTimeout;  // in second
   int64    lastSuccessTimeStamp;   // in second
   u_int32  lastBuildThreshold;     // in second 
   CurlTimeInfoT curlTime;

   JobInfoT* p_allJobs;
}GroupInfoT;

GroupInfoT* getTailGroup(GroupInfoT* p_headGroup);

// Parse argument from command line
bool parseArgument(int argc, char* argv[]);

// Parse Group
bool parseXMLFile(const char* fileName, GroupInfoT** pp_headGroup);
bool parseGroupAttr(xmlDoc *doc, xmlNode *groupNode, GroupInfoT* p_group);
void printAllGroupInfo(GroupInfoT* p_headGroup);
void printGroupInfo(GroupInfoT* p_group);
void initStuffOfAllGroup(GroupInfoT* p_headGroup);

// Parse Job 
bool parseJobsInfo(xmlDoc *doc, xmlNode *jobsNode, JobInfoT** p_headJob);
bool parseJobAttr(xmlDoc *doc, xmlNode *jobNode, JobInfoT* p_job);
void printAllJobInfo(JobInfoT* p_jobHead);
void printJobInfo(JobInfoT* p_job);

// Build Job Files
bool buildJobFiles(GroupInfoT* p_headGroup);

int64 timeStampFromFile(char* fileName);
int64 currentTimeStamp(void);
LedInfoT ledInfoFromfile(char* fileName);
void colorFromFile(char* fileName, char* colorStr, size_t strSize);
LedInfoT convert2LedInfo(char* colorStr);
char* convert2ColorString(LedInfoT led);

void initAllGroupLed(GroupInfoT* p_headGroup);
void ledControl(LedInfoT led, u_int8 red, u_int8 green, u_int8 blue);

// Build threads to Evaluate Color for each Group
bool buildEvalGrpColorTheads(GroupInfoT* p_headGroup);
void* evalGrpColorPoll(void* arg);
bool buildCurlCmd(GroupInfoT* p_group, char* curlCommand, u_int32 curlCommandSize);
bool executeCurlCmd(char* curlCommand);
void evaluateColor(GroupInfoT* p_group);
void evalGroupStatus(GroupInfoT* p_group);
void evalLedStatus(GroupInfoT* p_group);

// Build threads to control led for each group'
bool buildCtrlGrpLedThreads(GroupInfoT* p_headGroup);
void* ctrlGrpLedPoll(void* arg);
