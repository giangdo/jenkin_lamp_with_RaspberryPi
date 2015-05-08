#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include "jenkin_mon.h"

// Poll interval in each thread
#define CURL_POLL_INTERVAL 3
#define EVALUATE_COLOR_POLL_INTERVAL 2
#define DISPLAY_LED_POLL_INTERVAL 1

// Option to use authorized account to get info from jenkin server or not
#define USE_ANY_AUTHORIZED_IN_CURL 1

//----------------------------------------------------------------
// Global variable
//----------------------------------------------------------------
GroupInfoT* p_allGroups = NULL;
CurlInfoT* p_allCurls = NULL;

char* pathInfoFiles = "/opt/infoFiles";
char* xmlFile = "/opt/jobsJenkinConfig.xml";

// Option to control log
bool isVerbose = false;
// Option to control led
bool isControlRealLed = true;
bool isAllowAnime = true;
bool isDaemon = false;

/* Termination flag */
static int terminate = 0;
 
static void sig_term(int isig)
{
	terminate = 1;
}

static void sig_chld(int sig)
{
	/*
	 * This function will never be executed, however it is
	 * needed to make sure SIGCHLD is not set to SIG_IGN,
	 * in which case we can't wait() on any child.
	 */
}

//----------------------------------------------------------------------------
// Tail group: NULL or last have data inside but p_nextGroup = NULL
// Get tail group, tail group is the group in which memory was allocated but
// data inside is set to zero. So that p_nextGroup is NULL. Tail group can be
// understand as last empty data group. Head group can be tail group if there
// is no empty data inside it
//----------------------------------------------------------------------------
GroupInfoT* getTailGroup(GroupInfoT* p_headGroup)
{
   if (!p_headGroup)
   {
      return NULL;
   }
#if 0
   GroupInfoT* p_curGroup = p_headGroup;
   while (p_curGroup->p_nextGroup)
   {
      p_curGroup = p_curGroup->p_nextGroup;
   }
#else
   GroupInfoT* p_curGroup = NULL;
   for (p_curGroup = p_headGroup;
        p_curGroup->p_nextGroup;
        p_curGroup = p_curGroup->p_nextGroup);
#endif
   return p_curGroup;
}

void printAllJobInfo(JobInfoT* p_jobHead)
{
   JobInfoT* p_job = NULL;
   for (p_job = p_jobHead; p_job; p_job = p_job->p_nextJob)
   {
      printJobInfo(p_job);
   }
}

void printJobInfo(JobInfoT* p_job)
{
   if (p_job)
   {
      printf("job name: %s\n"\
             " job path: %s\n"\
             " status file: %s\n"\
             " last status file: %s\n",\
             p_job->jobName,
             p_job->jobPath,
             p_job->statusInfoFile,
             p_job->lastBuildInfoFile);
   }
}

//----------------------------------------------------------------------------
// Parsing Information of job (job attribute) in xml file to a JobInfoT data structure
//----------------------------------------------------------------------------
bool parseJobAttr(xmlDoc *doc, xmlNode *jobNode, JobInfoT* p_job)
{
   if (jobNode->children == NULL)
   {
      printf("Don't have any job attribute in this job in XML file\n");
      return false;
   }
   xmlNode* jobAttrNode = NULL;
   for (jobAttrNode = jobNode->children; jobAttrNode;
        jobAttrNode = jobAttrNode->next)
   {
      if (jobAttrNode->type == XML_ELEMENT_NODE)
      {
         if (!strcmp(jobAttrNode->name, "jobpath"))
         {
            p_job->jobPath =
               xmlNodeListGetString(doc, jobAttrNode->xmlChildrenNode, 1);
         }
         else if (!strcmp(jobAttrNode->name, "jobname"))
         {
            p_job->jobName = 
               xmlNodeListGetString(doc, jobAttrNode->xmlChildrenNode, 1);
         }
         else
         {
            printf("Wrong job attribute: %s\n", jobAttrNode->name);
            return false;
         }
      }
   }
   if (isVerbose)
   {
      printJobInfo(p_job);
   }
   return true;
}

//----------------------------------------------------------------------------
// Parsing Information of jobs in xml file to group data structure
//----------------------------------------------------------------------------
bool parseJobsInfo(xmlDoc *doc, xmlNode *jobsNode, JobInfoT** p_headJob)
{
   xmlNode* jobNode = NULL;
   if (!jobsNode->children)
   {
      printf("Don't have any job in group\n");
      return false;
   }
   JobInfoT* p_curJob = NULL;
   for (jobNode = jobsNode->children; jobNode; jobNode = jobNode->next)
   {
      if (jobNode->type == XML_ELEMENT_NODE) 
      {
         if (!strcmp(jobNode->name, "job"))
         {
            JobInfoT jobInfoTemp;
            memset(&jobInfoTemp, 0, sizeof(JobInfoT));
            if (parseJobAttr(doc, jobNode, &jobInfoTemp))
            { 
               if (!p_curJob)
               {
                  // First time -> head job = NULL
                  // Allocate memory for head job
                  p_curJob = malloc(sizeof(JobInfoT));
                  memset(p_curJob, 0, sizeof(JobInfoT));
                  *p_headJob = p_curJob; 
               }
               else
               {
                  // Allocate memory for next job
                  p_curJob->p_nextJob = malloc(sizeof(JobInfoT));
                  p_curJob = p_curJob->p_nextJob;
               }

               // Get all job data to current job 
               *p_curJob = jobInfoTemp;
               p_curJob->p_nextJob = NULL;
            }
            else
            {
               printf("Can not parse job attribute\n");
               return false;
            }
            
         }
         else
         {
            printf("Wrong ""job"" text: %s\n", jobNode->name);
            return false;
         }
      }
   }
   return true;
}

//----------------------------------------------------------------------------
// Parsing Information of Group in xml file to group data structure
//----------------------------------------------------------------------------
bool parseGroupAttr(xmlDoc *doc, xmlNode *groupNode, GroupInfoT* p_group)
{
   if (groupNode->children == NULL)
   {
      printf("Don't have any group attribute in this group in XML file\n");
      return false;
   }
   xmlNode* groupAttrNode = NULL;
   for (groupAttrNode = groupNode->children; groupAttrNode;
        groupAttrNode = groupAttrNode->next) 
   {
      if (groupAttrNode->type == XML_ELEMENT_NODE) 
      {
         if (!strcmp(groupAttrNode->name, "groupname"))
         {
            p_group->groupName =
               xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1);
         }
         else if (!strcmp(groupAttrNode->name, "server"))
         {
            p_group->serverName =
               xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1);
         }
         else if (!strcmp(groupAttrNode->name, "username"))
         {
            p_group->userName =
               xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1);
         }
         else if (!strcmp(groupAttrNode->name, "password"))
         {
            p_group->passWord =
               xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1);
         }
         else if (!strcmp(groupAttrNode->name, "red_led"))
         {
            p_group->redLed =
               atoi(xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1));
         }
         else if (!strcmp(groupAttrNode->name, "green_led"))
         {
            p_group->greLed =
               atoi(xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1));
         }
         else if (!strcmp(groupAttrNode->name, "blue_led"))
         {
            p_group->bluLed =
               atoi(xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1));
         }
         else if (!strcmp(groupAttrNode->name, "display_timeout"))
         {
            p_group->displaySuccessTimeout =
               atoi(xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1));
         }
         else if (!strcmp(groupAttrNode->name, "last_build_threshold"))
         {
            p_group->lastBuildThreshold=
               atoi(xmlNodeListGetString(doc, groupAttrNode->xmlChildrenNode, 1));
         }
         else if (!strcmp(groupAttrNode->name, "jobs"))
         {
            if(!parseJobsInfo(doc, groupAttrNode, &p_group->p_allJobs))
            {
               printf("Can not parse JobsInfo\n");
               return false;
            }
         }
         else
         {
            printf("Wrong group attribute: %s\n",groupAttrNode->name);
            return false;
         }
      }
   }
   if (isVerbose)
   {
      printGroupInfo(p_group);
   }
   return true;
}

//----------------------------------------------------------------------------
// Init led
//----------------------------------------------------------------------------
void initAllGroupLed(GroupInfoT* p_headGroup)
{
   GroupInfoT* p_group = NULL;
   for (p_group = p_headGroup; p_group; p_group = p_group->p_nextGroup)
   {
      p_group->ledStatus.color = WHI_COLOR;
      p_group->ledStatus.isAnime = false;
      ledControl(p_group->ledStatus,
                 p_group->redLed, p_group->greLed, p_group->bluLed);
      p_group->ledStatus.color = NON_COLOR;
      ledControl(p_group->ledStatus,
                 p_group->redLed, p_group->greLed, p_group->bluLed);
   }
}

//----------------------------------------------------------------------------
// Printf all Groups database
//----------------------------------------------------------------------------
void printAllGroupInfo(GroupInfoT* p_headGroup)
{
   GroupInfoT* p_group = NULL;
   for (p_group = p_headGroup; p_group; p_group = p_group->p_nextGroup)
   {
      printGroupInfo(p_group);
   }
}

//----------------------------------------------------------------------------
// Printf Group database
//----------------------------------------------------------------------------
void printGroupInfo(GroupInfoT* p_group)
{
   if (p_group)
   {
      printf("groupname: %s\n"\
            " server: %s\n"\
            " username: %s, password:%s\n"\
            " red: gpio%u, gre: gpio%u, blu: gpio%u\n"\
            " display_timeout: %u\n"\
            " last_build_threshold: %u\n",\
            p_group->groupName,
            p_group->serverName,
            p_group->userName, p_group->passWord,
            p_group->redLed, p_group->greLed, p_group->bluLed,
            p_group->displaySuccessTimeout,
            p_group->lastBuildThreshold);
      printAllJobInfo(p_group->p_allJobs);
   }
}

//----------------------------------------------------------------------------
// Parsing XML file and append data to All group database 
//----------------------------------------------------------------------------
bool parseXMLFile(const char* fileName, GroupInfoT** pp_headGroup)
{
   if (isVerbose)
   {
      printf("parseXMLFile: %s\n",fileName);
   }
   GroupInfoT* p_curGroup = NULL;
   xmlDoc* doc = NULL;
   xmlNode* rootNode = NULL;
   xmlNode* groupNode = NULL;
	LIBXML_TEST_VERSION
      
   if ((doc = xmlReadFile(fileName, NULL, 0)) == NULL)
   {
      printf("Can not xmlReadFile\n");
      return false;
   }
   
   // Get root node of XML file
   rootNode = xmlDocGetRootElement(doc);

   if (rootNode->children == NULL)
   {
      printf("Don't have any group in XML file\n");
      return false;
   }
   else
   {
      // Get start Posision of group database to loop
      p_curGroup = getTailGroup(*pp_headGroup);

      // Iterator to get information of all Groups node in XML file
      for (groupNode = rootNode->children; groupNode; groupNode = groupNode->next)
      {
         if (groupNode->type == XML_ELEMENT_NODE)
         {
            if(!strcmp(groupNode->name, "group"))
            {
               GroupInfoT groupInfoTemp;
               memset(&groupInfoTemp, 0, sizeof(groupInfoTemp));
               if (parseGroupAttr(doc, groupNode, &groupInfoTemp))
               {
                  if (!p_curGroup) 
                  {
                     // Allocate memory for head group 
                     p_curGroup = malloc(sizeof(GroupInfoT)); 
                     memset(p_curGroup, 0, sizeof(GroupInfoT));
                     *pp_headGroup = p_curGroup;
                  }
                  else
                  {
                     // Allocate memory for next group
                     p_curGroup->p_nextGroup = malloc(sizeof(GroupInfoT));
                     p_curGroup = p_curGroup->p_nextGroup;
                  }

                  // Get all group data temp to current Group
                  *p_curGroup = groupInfoTemp;
                  p_curGroup->p_nextGroup = NULL;
               }
               else
               {
                  printf("Cannot parse group in XML file\n");
                  return false;
               }
            }
            else
            {
               printf("wrong group XML element: %s\n",groupNode->name);
               return false;
            }
         }
      }
   }

	// Free the document
	xmlFreeDoc(doc);

	// Free the global variables that may have been allocated by the parser.
	xmlCleanupParser();

   return true;
} 

//----------------------------------------------------------------------------
// This function is use for parsing all argument in command line
//----------------------------------------------------------------------------
bool parseArgument(int argc, char* argv[])
{
   int indexArg; 
   for (indexArg = 1; indexArg < argc; indexArg++)
   {
      if (!strcmp(argv[indexArg], "-f") && (argc > indexArg + 1)) 
      {
        xmlFile = argv[++indexArg]; 
      }
      else if (!strcmp(argv[indexArg], "--verbose") ||
               !strcmp(argv[indexArg], "-v"))
      {
         isVerbose = true;
      }
      else if (!strcmp(argv[indexArg], "--realled") ||
               !strcmp(argv[indexArg], "-r"))
      {
         isControlRealLed = true;
      }
      else if (!strcmp(argv[indexArg], "--allowanime") ||
               !strcmp(argv[indexArg], "-a"))
      {
         isAllowAnime = true; 
      }
      else if (!strcmp(argv[indexArg], "--daemon") ||
               !strcmp(argv[indexArg], "-d"))
      {
         isDaemon = true;
      }
      else if (!strcmp(argv[indexArg], "--currentPath") ||
               !strcmp(argv[indexArg], "-c"))
      {
         pathInfoFiles = "./infoFiles";
      }
      else if (!strcmp(argv[indexArg], "--help") ||
          !strcmp(argv[indexArg], "-h") ||
          !strcmp(argv[indexArg], "help"))
      {
         printf("usage:\n"
                "default xml config file is /opt/jobsJenkinConfig.xml, if we want to change use -f\n"
                "default infoFiles directory is /opt/infoFiles, if we want to change to current path -> use -c\n"
                "./jenkin_mon\n"
                "./jenkin_mon -f configFILE.xml --currentPath --verbose --realled --allowanime --daemon\n"
                "./jenkin_mon -f configFILE.xml -c            -v        -r        -a           -d\n");
         exit(0);
      }
   }
   
   return true;
}

bool buildJobFiles(GroupInfoT* p_headGroup)
{
   GroupInfoT* p_group = p_headGroup;
   if (p_group)
   {
      u_int32 groupIndex = 0;
      for (; p_group; p_group = p_group->p_nextGroup)
      {
         JobInfoT* p_job = p_group->p_allJobs;
         if (p_job)
         {
            u_int32 jobIndex = 0;
            for (; p_job; p_job = p_job->p_nextJob)
            {
               sprintf(p_job->statusInfoFile, "%s/s%u_%u",
                       pathInfoFiles, groupIndex, jobIndex);
               sprintf(p_job->lastBuildInfoFile, "%s/l%u_%u",
                       pathInfoFiles, groupIndex, jobIndex);
               jobIndex++;
            }
            groupIndex++;
         }
         else
         {
            printf("Do not have any job in data base\n");
            return false;
         }
      }
   } 
   else
   {
      printf("Do not have group in data base\n");
      return false;
   }
   return true;
}

CurlInfoT* getMatchCurl(char* server, char* user, char* pass, CurlInfoT* p_headCurl)
{
   CurlInfoT* p_curl = NULL;
   CurlInfoT* p_matchCurl = NULL;
   for (p_curl = p_headCurl; p_curl; p_curl = p_curl->p_nextCurl)
   {
      if (!strcmp(p_curl->curlServer, server) &&
          !strcmp(p_curl->curlUser, user) &&
          !strcmp(p_curl->curlPass, pass))
      {
         p_matchCurl = p_curl;
         break;
      }
   }
   return p_matchCurl;
}

CurlInfoT* getLastCurl(CurlInfoT* p_headCurl)
{
   CurlInfoT* p_curl = NULL;
   for (p_curl = p_headCurl; p_curl->p_nextCurl; p_curl = p_curl->p_nextCurl);
   return p_curl;
}

void buildNewCurl(GroupInfoT* p_group, CurlInfoT* p_curl)
{
   p_curl->curlServer = p_group->serverName;
   p_curl->curlUser   = p_group->userName;
   p_curl->curlPass   = p_group->passWord;
#if USE_ANY_AUTHORIZED_IN_CURL 
   sprintf(p_curl->curlCommand, "curl -silent --anyauth");
#else
   sprintf(p_curl->curlCommand, "curl -silent -u %s:%s",
         p_curl->curlUser, p_curl->curlPass);
#endif
}

bool buildCurlCommand(GroupInfoT* p_headGroup, CurlInfoT** pp_headCurl)
{
   CurlInfoT* p_curl = *pp_headCurl;
   CurlInfoT* p_curlTemp = NULL;
   GroupInfoT* p_group = p_headGroup;
   if (p_group)
   {
      for (; p_group; p_group = p_group->p_nextGroup)
      {
         if (!p_curl)
         {
            // First time access to head Curl info
            p_curl = malloc(sizeof(CurlInfoT));
            buildNewCurl(p_group, p_curl);
            p_curl->p_nextCurl = NULL;
            *pp_headCurl = p_curl;
         }
         else
         {
            p_curlTemp = getMatchCurl(p_group->serverName, p_group->userName,
                                      p_group->passWord, *pp_headCurl);
            if (p_curlTemp)
            {
               // Use created curl infor
               p_curl = p_curlTemp;
            }
            else
            {
               // Create new curl infor in the end of curl database
               p_curl = getLastCurl(*pp_headCurl);
               p_curl->p_nextCurl = malloc(sizeof(CurlInfoT));
               p_curl = p_curl->p_nextCurl;
               buildNewCurl(p_group, p_curl);
               p_curl->p_nextCurl = NULL;
            }
         }

         JobInfoT* p_job = p_group->p_allJobs;
         if (p_job)
         {
            for (; p_job; p_job = p_job->p_nextJob)
            {
               // Build command to get status of Job
               char statusCmd[400];
#if 0
               sprintf(statusCmd,
                       " %s%s%s/api/json?pretty=true\"\\&\""\
                       "tree=name,color -o %s",
                       p_group->serverName, p_job->jobPath, p_job->jobName,
                       p_job->statusInfoFile); 
#else
               sprintf(statusCmd,
                       " %s%s%s/api/json?pretty=true\\&"\
                       "tree=name,color -o %s",
                       p_group->serverName, p_job->jobPath, p_job->jobName,
                       p_job->statusInfoFile); 
#endif
               // XXX TODO: Need to check strlen of statusCmd[] before concatenate this into
               // curlCommand[] because sizeof curlCommand[] only char curlCommand[20000]
               strcat(p_curl->curlCommand, statusCmd);

               // Build command to get last build information of Job
               char lastBuildCmd[400];
               sprintf(lastBuildCmd,
                       " %s%s%s/lastBuild/api/json?pretty=true\\&"\
                       "tree=fullDisplayName,id,timestamp,result -o %s",
                       p_group->serverName, p_job->jobPath, p_job->jobName,
                       p_job->lastBuildInfoFile); 
               strcat(p_curl->curlCommand, lastBuildCmd);
            }
         }
         else
         {
            printf("Do not have any job in data base\n");
            return false;
         }
      }
   } 
   else
   {
      printf("Do not have group in data base\n");
      return false;
   }
   return true;
}

//----------------------------------------------------------------------------
// Print all curl database
//----------------------------------------------------------------------------
void printAllCurlInfo(CurlInfoT* p_headCurl)
{
   CurlInfoT* p_curl = NULL;
   for (p_curl = p_headCurl; p_curl; p_curl = p_curl->p_nextCurl)
   {
      printf("curlCommand:\n%s\n", p_curl->curlCommand);

      // Implement first time curl command
      int curlResult = system(p_curl->curlCommand);
      printf("execute curl command result: %d\n", curlResult);
   }
}

//----------------------------------------------------------------------------
// Wait for all curl thread stop
//----------------------------------------------------------------------------
void waitAllCurlsThread(CurlInfoT* p_headCurl)
{
   CurlInfoT* p_curl = NULL;
   for (p_curl = p_headCurl; p_curl; p_curl = p_curl->p_nextCurl)
   {
      if (pthread_join(p_curl->curlThread, NULL))
      {
         printf("Can not join a curl thread\n");
      }
      else
      {
         if (isVerbose)
         {
            printf("Finish join a curl thread\n");
         }
      }
   }
}

//----------------------------------------------------------------------------
// Clean all curl database
//----------------------------------------------------------------------------
void cleanAllCurls(CurlInfoT* p_headCurl)
{
   CurlInfoT* p_curl = NULL;
   CurlInfoT* p_temp = NULL;
   for (p_curl = p_headCurl; p_curl; )
   {
      p_temp = p_curl->p_nextCurl;
      free(p_curl);
      p_curl = p_temp;
   }
   if (isVerbose)
   {
      printf("Finish clean all curl thread\n");
      printAllCurlInfo(p_allCurls);
   }
}

//----------------------------------------------------------------------------
// Poll to executing curl command
//----------------------------------------------------------------------------
void* curlThreadPoll(void *arg)
{
   CurlInfoT* p_curl = (CurlInfoT*)arg;
   
   strcat(p_curl->curlCommand,";echo done");
   while (!terminate)
   {
      // XXX : by using system() function, we can not receive SIGTERM or
      //       SIGINT signal by press "Ctrl + C"
      //       -> solution: use popen() function
#if 0
      int curlResult = system(p_curl->curlCommand);
      printf("execute curl command result: %d\n", curlResult);
#else
      char doneString[10];
      FILE* file;
      if ((file = popen(p_curl->curlCommand, "r")) == NULL)
      {
         printf("Can not execute command: %s\n",p_curl->curlCommand);
         return 0;
      }
      while ((fgets(doneString, sizeof(doneString), file) != NULL) && (!terminate));
      fclose(file);
      if (isVerbose)
      {
         printf("doneString: %s for server: %s\n", doneString, p_curl->curlServer);
      }
#endif
      sleep(CURL_POLL_INTERVAL);
   }

   printf("exit an curl thread poll for server: %s\n", p_curl->curlServer);
   return 0;
}

//----------------------------------------------------------------------------
// Build threads to execute curl commands.
// One thread will execute one curl command.
//----------------------------------------------------------------------------
bool buildCurlThreads(CurlInfoT* p_headCurl)
{
   CurlInfoT* p_curl = NULL;
   for (p_curl = p_headCurl; p_curl; p_curl = p_curl->p_nextCurl)
   {
      if(pthread_create(&p_curl->curlThread, NULL, curlThreadPoll, p_curl))
      {
         printf("Cannot create an curlThread, error: %s", strerror(errno));
         return false;
      }
   }
   return true;
}

//----------------------------------------------------------------------------
// Get color from status info file
//----------------------------------------------------------------------------
void colorFromFile(char* fileName, char* colorStr, size_t strSize)
{
   if (isVerbose)
   {
      printf("get color from file: %s\n", fileName);
   }
   char grepCommand[100];
   char colorLine[30];
   sprintf(grepCommand, "grep \"^  \\\"color\" %s", fileName);
   FILE* file = popen(grepCommand, "r");
   if (!file)
   {
      printf("can not openfile: %s, error: %s\n", fileName, strerror(errno));
      return; 
   }
   if (fgets(colorLine, sizeof(colorLine), file))
   { 
      if (sscanf(colorLine, "  \"color\" : \"%[^\"]", colorStr) != 1)
      {
         printf("get color failed.\n");
      }
   }
   fclose(file);
}

//----------------------------------------------------------------------------
// Get timestamp in second resolution from last build info file
//----------------------------------------------------------------------------
int64 timeStampFromFile(char* fileName)
{
   int64 timeStamp = 0;
   if (isVerbose)
   {
      printf("get timestamp from file: %s\n", fileName);
   }
   char grepCommand[100];
   char timeStampLine[50];
   char timeStampStr[30];
   sprintf(grepCommand, "grep \"timestamp\" %s", fileName);
   FILE* file = popen(grepCommand, "r");
   if (!file)
   {
      printf("can not openfile: %s, error: %s\n", fileName, strerror(errno));
      return 0;
   }
   if (fgets(timeStampLine, sizeof(timeStampLine), file))
   {
      //printf("timeStampLine: %s\n", timeStampLine);
                               //  \"timestamp\" : 1418372173536
      if (sscanf(timeStampLine, "  \"timestamp\" : %s", timeStampStr) != 1)
      {
         printf("get time stamp failed.\n");
      }
   }
   fclose(file);
   timeStamp = atoll(timeStampStr) / 1000;

   return timeStamp;
}

//----------------------------------------------------------------------------
// Get current from "date" command timestamp in second resolution 
//----------------------------------------------------------------------------
int64 currentTimeStamp(void)
{
   int64 timeStamp = 0;
#if 0
   char* dateCommand = "date +%s";
   char timeStampStr[30];
   FILE* file = popen(dateCommand, "r");
   if (!file)
   {
      printf("can not get timestamp\n");
      return timeStamp;
   }
   if (fgets(timeStampStr, sizeof(timeStampStr), file))
   {
      timeStamp = atoll(timeStampStr);
   }
   fclose(file);
#else
   struct timespec currentTime;
   if (clock_gettime(CLOCK_REALTIME, &currentTime) == -1)
   {
      printf("Can not get current time: %s\n", strerror(errno));
   }
   timeStamp = (int64)currentTime.tv_sec;
#endif
   return timeStamp;
}

//----------------------------------------------------------------------------
// Get led Status from color string
//----------------------------------------------------------------------------
LedInfoT convert2LedInfo(char* colorStr)
{
   LedInfoT led = {NON_COLOR, false};
   char* pAnimeStr = NULL;
   if ((pAnimeStr = strstr(colorStr, "_anime")) != NULL)
   {
      led.isAnime = true;

      //Strip "_anime" string
      memset(pAnimeStr, 0, strlen("_anime"));
   }
   else
   {
      led.isAnime = false;
   }

   Color2LedInfoT* pColor2Led = C2LInfo;
   while ((pColor2Led->color != NON_COLOR) &&
          (strcmp(colorStr, pColor2Led->colorStr))) 
   {
      pColor2Led++;
   }
   led.color = pColor2Led->color;
   return led;
}

//----------------------------------------------------------------------------
// Convert led Status to color string
//----------------------------------------------------------------------------
char* convert2ColorString(LedInfoT led)
{
   static char colorStr[20];
   Color2LedInfoT* pColor2Led = C2LInfo;
   while ((pColor2Led->color != NON_COLOR) &&
          (led.color != pColor2Led->color))
   {
      pColor2Led++;
   }
   strcpy(colorStr, pColor2Led->colorStr);
   if (led.isAnime)
   {
      strcat(colorStr, "_anime");
   }

   return colorStr;
}

//----------------------------------------------------------------------------
// Get led Status Info from file
//----------------------------------------------------------------------------
LedInfoT ledInfoFromfile(char* fileName)
{
   char colorStr[20];
   colorFromFile(fileName, colorStr, sizeof(colorStr));
   LedInfoT ledInfo = convert2LedInfo(colorStr);
   if (isVerbose)
   {
      printf("color: %d, isAnime %d\n", ledInfo.color, ledInfo.isAnime);
   }
   return ledInfo;
}

//----------------------------------------------------------------------------
// Evaluate led status for a group 
//----------------------------------------------------------------------------
void evalLedStatusForGroup(bool isAnime, bool isSuccess,
                           bool isThreshold, bool isAllDisable, GroupInfoT* p_group)
{
   if (isAllDisable)
   {
      p_group->ledStatus.color = NON_COLOR;
      p_group->ledStatus.isAnime = false;
      p_group->isFirstDisplaySuccess = true;
   }
   else
   {
      if (isThreshold)
      {
         p_group->ledStatus.color = YEL_COLOR;
         p_group->ledStatus.isAnime = false;
         p_group->isFirstDisplaySuccess = true;
      }
      else
      {
         if (isAnime)
         {
            p_group->ledStatus.color = YEL_COLOR;
            p_group->ledStatus.isAnime = true;
            p_group->isFirstDisplaySuccess = true;
         }
         else
         {
            if (isSuccess)
            {
               if (p_group->isFirstDisplaySuccess)
               {
                  // First time get success status
                  p_group->lastSuccessTimeStamp = currentTimeStamp();
                  p_group->ledStatus.color = BLU_COLOR;
                  p_group->ledStatus.isAnime = false;
                  p_group->isFirstDisplaySuccess = false;
               }
               else
               {
                  if (p_group->ledStatus.color == BLU_COLOR)
                  {
                     // Another time get success status
                     int64 curTime = currentTimeStamp();
                     printf("%lld , %lld\n", curTime - p_group->lastSuccessTimeStamp, (int64)p_group->displaySuccessTimeout);
                     if ((curTime - p_group->lastSuccessTimeStamp) >
                           (int64)p_group->displaySuccessTimeout)
                     {
                        p_group->ledStatus.color = NON_COLOR;
                        p_group->ledStatus.isAnime = false;
                     }
                  }
               }
            }
            else
            {
               p_group->ledStatus.color = RED_COLOR;
               p_group->ledStatus.isAnime = true;
               p_group->isFirstDisplaySuccess = true;
            }
         }
      } 
   }
}

//----------------------------------------------------------------------------
// Poll evaluate all color of all group
//----------------------------------------------------------------------------
void* evalColorThreadPoll(void* arg)
{
   GroupInfoT* p_group = NULL;
   JobInfoT* p_job = NULL;
   while (!terminate)
   {
      // Poll to all group to evaluate Led status for each group
      for (p_group = (GroupInfoT*)arg; p_group; p_group = p_group->p_nextGroup)
      {
         bool isAnime = false; 
         bool isSuccess = true;
         bool isThreshold = false;
         bool isAllDisable = true;
         for (p_job = p_group->p_allJobs; p_job; p_job = p_job->p_nextJob)
         {
            LedInfoT jobLedInfo = ledInfoFromfile(p_job->statusInfoFile);
            isAllDisable = isAllDisable &&
                           ((jobLedInfo.color == NO_BUILT) || (jobLedInfo.color == DISABLED));
            if ((jobLedInfo.color != NO_BUILT) &&
                (jobLedInfo.color != DISABLED)) 
            {
               isSuccess = isSuccess && (jobLedInfo.color == BLU_COLOR);
               isAnime = isAnime || jobLedInfo.isAnime; 

               int64 jobTime = timeStampFromFile(p_job->lastBuildInfoFile);
               int64 curTime = currentTimeStamp();
               isThreshold = isThreshold || 
                             ((curTime - jobTime) > (int64)p_group->lastBuildThreshold); 
            }
         }

         // Evaluate led status for a group
         evalLedStatusForGroup(isAnime, isSuccess, isThreshold, isAllDisable, p_group);
      }
      sleep(EVALUATE_COLOR_POLL_INTERVAL);
   }

   printf("exit evaluate color thread\n");
   return 0;
}

//----------------------------------------------------------------------------
// Read current value of GPIO
//----------------------------------------------------------------------------
GpioStatusE readGPIOValue(u_int8 pin)
{
   if (isControlRealLed)
   {
      GpioStatusE value;
      char catCommand[200];
      char valueStr[10];
      sprintf(catCommand, "cat /sys/class/gpio/gpio%u/value", pin);
      FILE* file;
      if (!(file = popen(catCommand, "r")))
      {
         printf("Can not read value of gpio: %s\n", catCommand);
         return;
      }
      if (fgets(valueStr, sizeof(valueStr), file))
      {
         value = atoi(valueStr);
      }
      fclose(file);
      return value;
   }
   else
   {
      return 0;
   }
}

//----------------------------------------------------------------------------
// Set state to GPIO after checking current state
//----------------------------------------------------------------------------
void setGPIOValue(u_int8 pin, GpioStatusE state)
{
   if (isControlRealLed)
   {
      if (state != readGPIOValue(pin))
      {
         char echoCommand[200];
         sprintf(echoCommand,
               "echo %d > /sys/class/gpio/gpio%u/value", state, pin);
         FILE* file;
         if (!(file = popen(echoCommand, "r")))
         {
            printf("Can not set value to gpio: %s\n", echoCommand);
            return;
         }
         fclose(file);
      }
   }
}

//----------------------------------------------------------------------------
// Set state to GPIO without checking current state
//----------------------------------------------------------------------------
void setGPIOValueNoCheck(u_int8 pin, GpioStatusE state)
{
   if (isControlRealLed)
   {
      char echoCommand[200];
      sprintf(echoCommand,
            "echo %d > /sys/class/gpio/gpio%u/value", state, pin);
      FILE* file;
      if (!(file = popen(echoCommand, "r")))
      {
         printf("Can not set value to gpio: %s\n", echoCommand);
         return;
      }
      fclose(file);
   }
}

//----------------------------------------------------------------------------
// Control led 
//----------------------------------------------------------------------------
void ledControl(LedInfoT led, u_int8 red, u_int8 green, u_int8 blue)
{
   Color2LedInfoT* pColor2Led = C2LInfo;
   while ((pColor2Led->color != NON_COLOR) &&
         (led.color != pColor2Led->color))
   {
      pColor2Led++;
   }

   GpioStatusE r = OF;
   GpioStatusE g = OF;
   GpioStatusE b = OF;

   if (isAllowAnime && led.isAnime)
   {
      GpioStatusE currentState;
      if (pColor2Led->r == ON)
      {
         currentState = readGPIOValue(red);
      }
      else if (pColor2Led->g == ON)
      {
         currentState = readGPIOValue(green);
      }
      else if (pColor2Led->b == ON)
      {
         currentState = readGPIOValue(blue);
      }
      else
      {
         currentState = readGPIOValue(red);
      }

      if (pColor2Led->r == ON)
      {
         r = !currentState;
      }

      if (pColor2Led->g == ON)
      {
         g = !currentState;
      }

      if (pColor2Led->b == ON)
      {
         b = !currentState;
      }
   }
   else
   {
      r = pColor2Led->r;
      g = pColor2Led->g;
      b = pColor2Led->b;
   }

   if (isControlRealLed)
   {
      setGPIOValueNoCheck(red  , r);
      setGPIOValueNoCheck(green, g);
      setGPIOValueNoCheck(blue , b);
   }
   else
   {
      printf("red-green-blue: %d-%d-%d r-g-b:%d-%d-%d\n", red, green, blue, r, g, b);  
   }
}

//----------------------------------------------------------------------------
// Poll to display all color of all group
//----------------------------------------------------------------------------
void* displayLedThreadPoll(void* arg)
{
   GroupInfoT* p_group = NULL;
   while (!terminate)
   {
      for (p_group = (GroupInfoT*)arg; p_group; p_group = p_group->p_nextGroup)
      {
         printf("group %s: %s\n", p_group->groupName,
                convert2ColorString(p_group->ledStatus));
         ledControl(p_group->ledStatus,
                    p_group->redLed, p_group->greLed, p_group->bluLed);
      }
      sleep(DISPLAY_LED_POLL_INTERVAL);
   }

   printf("exit display led thread\n");
   return 0;
}

//----------------------------------------------------------------------------
// This function is use for parsing all argument in command line
//----------------------------------------------------------------------------
int main(int argc, char *argv[])
{
	int job_count = 0, i;
	struct thread_info *job_thread_tmp;
	struct job_data *job_data_tmp;
	char date[8];
	memset(date, 0, sizeof(date));

   // Parse Argument from command line
   if (!parseArgument(argc, argv))
   {
      printf("Can not parse command line!\n");
      exit(0);
   }

   // Daemonize
   if (isDaemon)
   {
      switch (fork())
      {
         case 0:
            break;
         case -1:
            fprintf(stderr, "fork failed");
            exit(1);
         default:
            exit(0);
      }
      if (setsid() == -1)
      {
         fprintf(stderr, "setsid failed");
         exit(1);
      }
      close(0);
      close(1);
      close(2);
   }

	/* Make sure SIGCHLD is not SIG_IGN */
	while (signal(SIGCHLD, sig_chld) == SIG_ERR) {
		printf("signal() failed: %s", strerror(errno));
	}

	/* Catch SIGTERM */
	while (signal(SIGTERM, sig_term) == SIG_ERR) {
		printf("signal() failed: %s", strerror(errno));
	}

	/* Catch SIGINT */
	while (signal(SIGINT, sig_term) == SIG_ERR) {
		printf("signal() failed: %s", strerror(errno));
	}
   
   // Parse XML file
   if (!parseXMLFile(xmlFile, &p_allGroups))
   {
      printf("Can not parse XML file\n");
      exit(0);
   }

   // Build files to store data get from curl commands for each jobs
   if (!buildJobFiles(p_allGroups))
   {
      printf("Can not buildJobFiles\n");
      exit(0);
   }

   printAllGroupInfo(p_allGroups);
   
   // Init all LED of All groups
   initAllGroupLed(p_allGroups);

   // Build "curl" commands to get status of jobs
   if (!buildCurlCommand(p_allGroups, &p_allCurls))
   {
      printf("Can not buildCurlCommand\n");
      exit(0);
   }
   else
   {
      printAllCurlInfo(p_allCurls);
   }

   // Build Threads for executing "curl" commands
   if (!buildCurlThreads(p_allCurls))
   {
      printf("Can not buildCurlThreads\n");
   }

   // Build thread for evaluate Color for each group
   pthread_t evalColorThread;
   if (pthread_create(&evalColorThread, NULL, evalColorThreadPoll, p_allGroups))
   {
      printf("Can not build Evalute Color Thread\n");
      exit(0);
   }

   // Build thread for display LED
   pthread_t displayLedThread;
   if (pthread_create(&displayLedThread, NULL, displayLedThreadPoll, p_allGroups))
   {
      printf("Can not build Display Led Thread\n");
   }

   while(!terminate);
   
   // Waiting for all curl Threads stop
   waitAllCurlsThread(p_allCurls);

   // Clean all curl database
   cleanAllCurls(p_allCurls);

   // Waiting for evalute Color Thread stop
   if (pthread_join(evalColorThread, NULL))
   {
      printf("Can not join evaluate color thread\n");
   }

   // Waiting for display Led Thread stop
   if (pthread_join(displayLedThread, NULL))
   {
      printf("Can not join display led thread\n");
   }

   // Clean all Group and job database
   // XXX todo
	return 0;
}
