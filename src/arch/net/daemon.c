/*****************************************************************************
 * $Source$
 * $Author$
 * $Date$
 * $Revision$
 *****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <sys/types.h>
#include <process.h>
#else
#include <errno.h>
typedef unsigned int BYTE;
#endif
#include <time.h>

#include "sockRoutines.h"
#include "daemon.h"

/*If FACELESS is defined, the daemon logs runs to a file
(daemon.log) and doesn't show a DOS window.
Otherwise, the daemon logs things to its DOS window.
*/
/*#define FACELESS  /*<- sent in from the makefile*/

FILE *logfile;/*Status messages to standard output*/

int abort_writelog(int code,const char *msg) {
	fprintf(logfile,"Socket error %d-- %s!\n",code,msg);
	fclose(logfile);
	exit(3);
	return -1;
}

char startProgram(const char *exeName, const char *args, 
				const char *cwd, char *env);

void goFaceless(void);

int main()
{
  unsigned int myPortNumber = DAEMON_IP_PORT;
  int myfd;
  
  int remotefd;         /* Remote Process Connecting */
  skt_ip_t remoteIP;         /* Remote Process's IP */
  unsigned int remotePortNumber; /* Port on which remote port is connecting */
  
  taskStruct task;      /* Information about the task to be performed */
  time_t curTime;

  logfile=stdout;
#ifdef FACELESS
  logfile=fopen("daemon.log","w+");
  if (logfile==NULL) /*Couldn't open log file*/
    logfile=stdout;
  else 
	goFaceless();
#endif
  
  curTime=time(NULL);
  fprintf(logfile,"Logfile for Windows Spawner Daemon for Converse programs\n"
	  "Run starting: %s\n\n",ctime(&curTime));fflush(logfile);
  
  skt_init();
  skt_set_abort(abort_writelog);
  
  /* Initialise "Listening Socket" */
  myfd=skt_server(&myPortNumber);
  
  while(1) {
    char ip_str[200];
    char *argLine; /* Argument list for called program */
    char statusCode;/*Status byte sent back to requestor*/
    
    fprintf(logfile,"\nListening for requests on port %d\n",myPortNumber);
	fflush(logfile);
    
    /* Accept & log a TCP connection from a client */
    remotefd=skt_accept(myfd, &remoteIP, &remotePortNumber); 
    
    curTime=time(NULL);
    fprintf(logfile,"Connection from IP %s, port %d at %s",
	    skt_print_ip(ip_str,remoteIP),remotePortNumber,
	    ctime(&curTime));
    fflush(logfile);
    
    /* Recv the task to be done */
    skt_recvN(remotefd, (BYTE *)&task, sizeof(task));
    task.pgm[DAEMON_MAXPATHLEN-1]=0; /*null terminate everything in sight*/
    task.cwd[DAEMON_MAXPATHLEN-1]=0;
    task.env[DAEMON_MAXENV-1]=0;

    /* Check magic number */
    if (ChMessageInt(task.magic)!=DAEMON_MAGIC) {
      fprintf(logfile,"************ SECURITY ALERT! ************\n"
	      "Received execution request with the wrong magic number 0x%08x!\n"
	      "This could indicate someone is trying to hack your system.\n\n\n",ChMessageInt(task.magic));
      fflush(logfile);
      skt_close(remotefd);
      continue; /*DON'T execute this command (could be evil!)*/
    }
    
    /* Allocate memory for arguments*/
    argLine = (char *)malloc(sizeof(char) * (ChMessageInt(task.argLength)+1));
    
    /* Recv the command line*/
    skt_recvN(remotefd, (BYTE *)argLine, ChMessageInt(task.argLength));
    
    /*Add null terminator*/
    argLine[ChMessageInt(task.argLength)] = 0;
    
    fprintf(logfile,"Invoking '%s'\n"
	    "and environment '%s'\n"
	    "in '%s'\n",
	    task.pgm,task.env,task.cwd);fflush(logfile);
    
    /* Finally, create the process*/
    statusCode=startProgram(task.pgm,argLine,task.cwd,task.env);

    /*Send status byte back to requestor*/
    skt_sendN(remotefd,(BYTE *)&statusCode,sizeof(char));
    skt_close(remotefd);

    /*Free recv'd arguments*/
    free(argLine);
  }
  return 0;  
}

#ifdef _WIN32
/********************** Win32 Spawn ****************/
void goFaceless(void)
{
    printf("Switching to background mode...\n");
	fflush(stdout);
    sleep(2);/*Give user a chance to read message*/
    FreeConsole();
}
/*
Paste the environment string oldEnv just after dest.
This is a bit of a pain since windows environment strings
are double-null terminated.
  */
void envCat(char *dest,LPTSTR oldEnv)
{
  char *src=oldEnv;
  dest+=strlen(dest);/*Advance to end of dest*/
  dest++;/*Advance past terminating NULL character*/
  while ((*src)!='\0') {
    int adv=strlen(src)+1;/*Length of newly-copied string plus NULL*/
    strcpy(dest,src);/*Copy another environment string*/
    dest+=adv;/*Advance past newly-copied string and NULL*/
    src+=adv;/*Ditto for src*/
  }
  *dest='\0';/*Paste on final terminating NULL character*/
  FreeEnvironmentStrings(oldEnv);
}


char startProgram(const char *exeName, const char *args, 
				const char *cwd, char *env)
{
  int ret;
  PROCESS_INFORMATION pi;         /* process Information for the process spawned */
  STARTUPINFO si={0};                 /* startup info for the process spawned */

  char environment[10000];/*Doubly-null terminated environment strings*/
  char cmdLine[10000];/*Program command line, including executable name*/
  if (strlen(exeName)+strlen(args) > 10000) 
	return 0; /*Command line too long.*/
  strcpy(cmdLine,exeName);
  strcat(cmdLine," ");
  strcat(cmdLine,args);

  /*Copy over the environment variables*/
  strcpy(environment,env);
  /*Paste all system environment strings after task.env */
  envCat(environment,GetEnvironmentStrings());
  
  /* Initialise the security attributes for the process 
     to be spawned */
  si.cb = sizeof(si);   

  ret = CreateProcess(NULL,	/* application name */
			    cmdLine,	/* command line */
			    NULL,/*&sa,*/							/* process SA */
			    NULL,/*&sa,*/							/* thread SA */
			    FALSE,							/* inherit flag */
#ifdef FACELESS
				CREATE_NEW_PROCESS_GROUP|DETACHED_PROCESS, 
#else
				CREATE_NEW_PROCESS_GROUP|CREATE_NEW_CONSOLE,
#endif
				/* creation flags */
			    environment,					/* environment block */
			    cwd,							/* working directory */
			    &si,							/* startup info */
			    &pi);
 
  if (ret==0)
  {
      /*Something went wrong!  Look up the Windows error code*/
      int error=GetLastError();
      char statusCode=daemon_err2status(error);
      fprintf(logfile,"******************* ERROR *****************\n"
	      "Error in creating process!\n"
	      "Error code = %ld-- %s\n\n\n", error,
	      daemon_status2msg(statusCode));
	  fflush(logfile);
	  return statusCode;
  } 
  else
	return 'G';
}

#else /*UNIX systems*/

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

void goFaceless(void)
{
    printf("Switching to background mode...\n");
	if (fork()!=0) 
		exit(0); /*Kill off the parent process, freeing terminal*/
}

char ** args2argv(const char *args,char **argv) {
	int cur=0,len=strlen(args);
	int argc=0;
	while (cur<len) {
		int start,end;
		while (cur<len && args[cur]==' ') cur++;
		start=cur;
		while (cur<len && args[cur]!=' ') cur++;
		end=cur;
		if (start<end){
			int i;
			argv[argc]=(char *)malloc(sizeof(char)*(end-start+1));
			for (i=0;i<end-start;i++)
				argv[argc][i]=args[start+i];
			argv[argc++][end-start]=0;/*Null-terminate*/
		}
	}
	argv[argc]=0;/* Null-terminate arg list */
	return argv;
}



char startProgram(const char *exeName, const char *args, 
				const char *cwd, char *env)
{
	int ret=0,fd;
	if (0!=access(cwd,F_OK)) return 'D';
	if (0!=access(cwd,X_OK)) return 'A';
	if (0!=access(exeName,F_OK)) return 'F';
	if (0!=access(exeName,R_OK|X_OK)) return 'A';
	
	if (fork()==0)
	{
		char **argv=(char **)malloc(sizeof(char *)*1000);
		ret|=chdir(cwd);
		putenv(env);
#ifdef FACELESS
		/*Redirect program's stdin, out, err to /dev/null*/
		fd=open("/dev/null",O_RDWR);
		dup2(fd,0);
		dup2(fd,1);
		dup2(fd,2);
#endif
		for (fd=3;fd<1024;fd++) close(fd);
		ret|=execvp(exeName,args2argv(args,argv));
	}
	/*FIXME: parent needs to check on child's status, e.g., by 
	  child sending SIGUSR2 back to parent on error.*/
	return 'G';
}

#endif
