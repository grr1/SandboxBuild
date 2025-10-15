/*
 * A one-pass parser for the output of strace -f
 *
 * A line of output from strace -f is in the format [PID] [syscall]("filepath_to_executable", [arg1, arg2, ... ])
 * This parser looks for lines in which the following conditions are met:
    1: the system call performed is "execve"
    2: the ending of the executable is "gcc", "g++", "ld", or "as"
 * These such lines 

 */

#include <errno.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>


// constant for large buffer lengths
#define BUFFER_SIZE 512

/*
 * Linked list node struct for dependency files for one target
 */
typedef struct depnode_struct {
  char *dep; //dependency filepath
  struct depnode_struct *next;
}  depnode;

/*
 * Contains information about one make target
 */
typedef struct targetstruct {
  char *target_name;
  char *cmd;
  depnode *head;
  depnode *tail;
} target;

/*
 * Adds a new dependency filepath to a target
 */
void TARGET_add_dep(target *tar, char *new_dep) {
  depnode *copy = tar->head;
  while ( copy != NULL ) {
    if ( !strcmp(copy->dep, new_dep ) ) {
      // target already has this dependency, do not repeat it
      return;
    }
    copy = copy->next;
  }
  depnode *newnode = malloc(sizeof(depnode));
  newnode->dep = strdup(new_dep);
  newnode->next = NULL;
  if ( tar->head == NULL ) {
    tar->head = tar->tail = newnode;
  }
  else {
    tar->tail->next = newnode;
    tar->tail = newnode;
  }
}

/*
 * Emits the information needed to build one target to the generated sandbox makefile
 * params:
 *    file: the file pointer to the generated makefile in the sandbox dir
 *    sb_pwd: the filepath to the sandbox, used to insert -I flag in gcc cmds
 *    tar:  pointer to the target struct containing the information to be writen
 */
void emit_target_to_makefile(FILE *file, char *sb_pwd, target *tar) {
  // first file is the local dependency
  // ex: target: target.cc
  fprintf(file, "\n%s: %s\n", tar->target_name, tar->head->dep);
  // write the command to execute for this target
  //TODO: need to change to track multiple commands
  //TODO: write in "-I[path-to-sandbox] for gcc commands
  //      to add sandbox directory to the linking path
  char *gcc_index = strstr(tar->cmd, "gcc");
  if ( !gcc_index ) {
    //if it is not a gcc command, check for a g++ command
    gcc_index = strstr(tar->cmd, "g++");
  }
  if ( gcc_index ) {
    //write all chars up to and including "gcc " in the command
    fprintf(file, "\t");
    fwrite(tar->cmd, 1, gcc_index - tar->cmd + 4, file);
    fprintf(file, "-I%s %s\n", sb_pwd, gcc_index + 4);
  }
  else {
    fprintf(file, "\t%s\n",tar->cmd); 
  }
}

/*
 * Emits information for one target and its command and dependencies
 * to the dependency.txt file
 */
void emit_target_to_file( FILE *file, target *tar ) {
  fprintf(file, "TARGET:  %s\n", tar->target_name);
  fprintf(file, "COMMAND:  %s\n", tar->cmd);
  fprintf(file, "DEPENDENCY:");
  // output all dependencies for this target
  depnode *copy = tar->head;
  int line_len = 12;
  do {
    // formatting
    if ( line_len + strlen(copy->dep) > 80 ) {
      fprintf(file, "\n            ");
      line_len = 12;
    }
    fprintf(file, "  %s", copy->dep);
    line_len += strlen(copy->dep) + 2;
    copy = copy->next;
  } while ( copy != NULL );
  fprintf(file, "\n");
}

/*
 * Helper function for creating subdirectories recursively from a given filepath
 * dirpath: the absolute filepath of the the dependency to be copied from
 * sandboxDir: the absolute filepath of the sandbox directory to copy to
 */
void dep_mkdirs(char *dirpath, char *sandboxDir) {

  char *full_path = malloc(strlen(dirpath) + strlen(sandboxDir) + 100);
  strcpy(full_path, sandboxDir);
  strcat(full_path, dirpath);
  struct stat statbf;
  char *dirpath_cpy = strdup(dirpath);
  char *dname = dirname(dirpath_cpy);
  if ( strcmp(dname, ".") &&
       ( stat(dname, &statbf) != 0 || !S_ISDIR(statbf.st_mode) ) ) {
    //recursively make the parent directories before making this directory
    dep_mkdirs(dname, sandboxDir);
  }
  free(dirpath_cpy);
  int ret = mkdir(dirpath, 0777);
  free(full_path);
}

/*
 * Helper function to create copies of the dependency files for the given
 * target in the given sandbox directory
 */
void TARGET_copy_deps(target *tar, char *sandbox_pwd) {
  depnode *copy = tar->head;
  while ( copy != NULL ) {
    //fprintf(stderr, "DEP FILE: %s+\n", copy->dep);
    // the original source dependency to copy from
    FILE *depfile = fopen(copy->dep, "r");
    if ( depfile == NULL ) {
      fprintf(stderr, "ERROR: Dependency file %s could not be opened to copy!\n", copy->dep);
      copy = copy->next;
      continue;
    }
    // create a new copy of the dependency file to write to
    // pwd/dep
    char *new_path = malloc(strlen(sandbox_pwd) + 2 + strlen(copy->dep));
    strcpy(new_path, sandbox_pwd);
    if ( copy->dep[0] != '/') {
      *(new_path + strlen(sandbox_pwd)) = '/';
      *(new_path + strlen(sandbox_pwd) + 1) = '\0';
    }
    // append dep filepath onto pwd to create abs filepath
    // for the sandbox copy
    strcat(new_path, copy->dep);
    *(new_path + strlen(sandbox_pwd) + strlen(copy->dep) + 1) = '\0';
    //fprintf(stderr, "NEW PATH: %s+\n", new_path);
    //create subdirs if not exist alr
    if ( strcmp(basename(new_path), new_path) ) {
      //dependency has a directory in its filepath, need to check if those directories exist
      struct stat stat_result;
      char *new_path_cpy = strdup(new_path);
      char *copy_dname = dirname(new_path_cpy);
      if ( stat(copy_dname, &stat_result) != 0 || !S_ISDIR(stat_result.st_mode) ) {
        //subdir in sandbox does not exist, need to make it
        dep_mkdirs(copy_dname, sandbox_pwd);
      }
      free(new_path_cpy);
    }
    FILE *towrite = fopen(new_path, "w");
    if ( towrite == NULL ) {
      fprintf(stderr, "ERROR: Sandbox copy, %s, of dependency %s could not be opened!\n\n",
                new_path, copy->dep);
      copy = copy->next;
      continue;
    }
    // copy from the dependency file to the towrite copy
    char *read_buffer = malloc(BUFFER_SIZE);
    int bytes_read = -1;
    do {
      //read 512 items of 1 byte each
      bytes_read = fread(read_buffer, 1, BUFFER_SIZE, depfile);
      fwrite(read_buffer, 1, bytes_read, towrite);
    } while ( bytes_read > 0);
    free(read_buffer);
    fclose(depfile);
    fclose(towrite);
    copy = copy->next;
  }
}

/*
 * linked list node struct to hold a process id and its associated filepath
 */
typedef struct list_node {
  int pid; // pid of current process
  char *path;
  struct list_node *next;
} node;

/*
 * Linked list struct
 */
typedef struct linked_list {
  node *head;
  node *tail;
} list;

/*
 * Helper function to find the filepath associated with a particular pid.
 * Iterates across all nodes in the linked list.and returns the node
 * with the matching pid key.
 */
node *LIST_find_pid(list *list_in, int pid) {
  node *cur = list_in->head;
  while ( cur != NULL ) {
    if ( cur->pid == pid ) {
      return cur;
    }
    cur = cur->next;
  }
  return NULL;
}

/*
 * Helper function to add a node to the linked list.
 * Uses LIST_find_pid() to check for pre-existence in the linked list.
 */
void LIST_add(list *fp_list,int pid, char *filepath) {
  // do not add to list if node with matching pid already exists
  node *existing_node = LIST_find_pid(fp_list, pid);
  if ( existing_node == NULL ) {
    node *new_node = malloc(sizeof(node));
    new_node->pid = pid;
    new_node->path = filepath;
    new_node->next = NULL;
    if ( fp_list->head == NULL ) {
      fp_list->head = fp_list->tail = new_node;
    }
    else {
      fp_list->tail->next = new_node;
      fp_list->tail = new_node;
    }
  }
  else {
    // matching pid exists in list, update its fp
    existing_node->path = filepath;
  }
}

/*
 * Helper function to parse the name of the target executablefile from a gcc/g++ command
 * Examples:
 * - Command:         gcc -o output source.c
 *   Target File is:  output
 * - Command:         g++ -o otheroutput othersource
 *   Target File is:  otheroutput
 */
char * parse_target_from_cmd(char *cmd) {
  //create a copy to not put null terminator in the original command argument
  char *target = strstr(cmd, "-o ");
  char *target_copy = strdup(target) + 3; // cut off "-o "
  int index = 0;
  while ( target_copy[index] != ' ' ) {
    index++;
  }
  target_copy[index] = '\0';
  return target_copy;
}

/*
 * Helper function to check if a given command is one of the desired commands
 */
bool is_desired_cmd(char *cmd) {
  return !strcmp(cmd, "gcc") || !strcmp(cmd, "g++") ||
         !strcmp(cmd, "as" )  || !strcmp(cmd, "ld") ;
}

/*
 * Helper function to extract source c/c++, .s, and .o file names from a line
 */
char *extract_sources(char *line) {
  // check for .cc files first so the check for .c does not match them
  char *fname = strstr((const char *) line, ".cc");
  if ( fname != NULL ) {
    int fname_len = 3;
    //decrement the pointer until reaching a space
    for ( int i = 0; i < fname - line ; i++ ) {
      if ( *(fname - 1) == '\"' ) { // reached the first char of the file name
        break;
      }
      fname--;
      fname_len++;
    }
    // create cooy of fname to null terminate without corrupting the original string
    char *fname_copy = strdup(fname);
    *(fname_copy + fname_len) = '\0';
    return fname_copy;
  }
  // check for .c files
  fname = strstr((const char *) line, ".c");
  if ( fname != NULL ) {
    int fname_len = 2;
    //decrement the pointer until reaching a space
    for ( int i = 0; i < fname - line ; i++ ) {
      if ( *(fname - 1) == '\"' ) { // reached the first char of the file name
        break;
      }
      fname--;
      fname_len++;
    }
    // create copy of fname to null terminate without corrupting the original string
    char *fname_copy = strdup(fname);
    *(fname_copy + fname_len) = '\0';
    return fname_copy;
  }
  // check for .o files
  fname = strstr((const char *) line, ".o");
  if ( fname != NULL ) {
    int fname_len = 2;
    //decrement the pointer until reaching a space
    for ( int i = 0; i < fname - line ; i++ ) {
      if ( *(fname - 1) == '\"' ) { // reached the first char of the file name
        break;
      }
      fname--;
      fname_len++;
    }
    // create copy of fname to null terminate without corrupting the original string
    char *fname_copy = strdup(fname);
    *(fname_copy + fname_len) = '\0';
    return fname_copy;
  }
  // check for .s files
  fname = strstr((const char *) line, ".s");
  if ( fname != NULL ) {
    int fname_len = 2;
    //decrement the pointer until reaching a space
    for ( int i = 0; i < fname - line ; i++ ) {
      if ( *(fname - 1) == '\"' ) { // reached the first char of the file name
        break;
      }
      fname--;
      fname_len++;
    }
    // create copy of fname to null terminate without corrupting the original string
    char *fname_copy = strdup(fname);
    *(fname_copy + fname_len) = '\0';
    return fname_copy;
  }
}

// the output of the strace call will be found in t.out
const char *input_file_name = "t.out";
//the list of commands used to make the build will be written to commands_cache.txt
const char *cmds_file_name = "commands_cache.txt";
//the list of c and c++ sourcefiles used to make the build will be written to c_cpp_files.txt
const char *sources_file_name = "source_files.txt";
/* the dependency file: lists commands, sources, and dependencies in the following format
 * OUTPUT: program/object/file/library
 * COMMAND: gcc -o ...
 * DEPENDENCY: dep1.c dep2.h dep3.cc ....
 */
const char *dependency_file_name = "dependency.txt";


int main(int argc, char *argv) {
  // argv: "record-build" [targets]
  // execvp("/usr/bin/strace", ["/usr/bin/strace", "-f", "-o", "t.out", "make", [targets]);
  // arguments for execve
  char *exec_args[argc + 4];
  exec_args[0] = "/usr/bin/strace";
  exec_args[1] = "-f";
  exec_args[2] = "-o";
  exec_args[3] = "t.out";
  exec_args[4] = "make";
  for ( int i = 1; i < argc; i++ ) {
    exec_args[i + 4] = &(argv[i]);
  }

  // fork a child process to execute strace in
  int ret = fork();
  if ( ret == 0 ) {
    execvp(exec_args[0], exec_args);
  }
  // wait for the forked process to complete
  waitpid(ret, NULL, 0);

  //open input file for writing
  FILE *in_file = fopen(input_file_name, "r");
  if (in_file == NULL ) {
    //check for fopen failure
    fprintf(stderr, "ERROR: input file to be parsed,  %s, could not be opened!\n", input_file_name);
    exit(1);
  }

  //open file to write list of commands to
  FILE *cmds_file = fopen(cmds_file_name, "w");
  if (cmds_file == NULL ) {
    //check for fopen failure
    fprintf(stderr, "ERROR: file to write list of commands to,  %s, could not be opened!\n",cmds_file_name);
    //close input file
    fclose(in_file);
    exit(1);
  }

  //open file to write list of source files to
  FILE *sources_file = fopen(sources_file_name, "w");
  if (sources_file == NULL ) {
    //check for fopen failure
    fprintf(stderr, "ERROR: file to write source file names to,  %s, could not be opened!\n", sources_file_name);
    //close input file and command file
    fclose(in_file);
    fclose(cmds_file);
    exit(1);
  }

  FILE *dep_file = fopen(dependency_file_name, "w");
  if ( dep_file == NULL ) {
    //check for open failure
    fprintf(stderr, "ERROR: file to write dependencies to, %s, could not be opened\n", dependency_file_name);
  }

  char buffer[BUFFER_SIZE]; //buffer to hold a line in
  char args[BUFFER_SIZE]; //buffer to hold the arguments of an execve call in
  int pid = -1; //the pid of the system call on the current line
  bool vfork = false; // was the previous line a vfork call?
                      // if so, this line is in that child process
  int saved_pid = -1;

  // linked list to hold the filepaths of desired commands
  list *fps_list = malloc(sizeof(list));

  // get the current working directory, to list absolute filepaths in
  char *pwd = malloc(BUFFER_SIZE);
  if (pwd == NULL ) {
    fprintf(stderr, "PWD MALLOC FAIL\n");
    exit(1);
  }
  //TODO: figure out where to free this, memory leak possible with pwd
  getcwd(pwd, BUFFER_SIZE);
  
  // keep track of the line length for the DEPENDENCY entries for each make target
  // used for formatting dependency file
  int dep_length = 12;

  // the current target struct node ptr
  // used to remove repeated dependencies, and for copying into sandbox
  target *cur_target = NULL;

  // create a new directory for the sandbox dependencies to be copied into
  char *sandbox_pwd = malloc(strlen(pwd) + 9);
  strcpy(sandbox_pwd, pwd);
  strcat(sandbox_pwd, "/");
  strcat(sandbox_pwd, "sandbox");
  int status = mkdir(sandbox_pwd, 0777);

  //create makefile inside the sandbox
  char *sandbox_mkfile_path = strdup(sandbox_pwd);
  strcat(sandbox_mkfile_path, "/Makefile");
  FILE* sandbox_mkfile = fopen(sandbox_mkfile_path, "w");
  if ( !sandbox_mkfile ) {
    fprintf(stderr, "Sandbox makefile, \"%s\", could not be opened for writing!",
              sandbox_mkfile_path);
  }
  else {
    //write the wrapper for all targets to the makefile
    //  all_make_targets is a special generated target that will have dependencies on all
    //  other targets, and will be placed at the end, to allow the 'make' command to
    //  build all of the targets based on building the 'all' target on the first line
    fprintf(sandbox_mkfile, "\nall: all_make_targets\n");
  }

  //buffer to track all of the targets made by this build
  char make_targets_list[BUFFER_SIZE];

  //read one line in and compare it with the target format
  while(!feof(in_file) && fgets(buffer, sizeof(buffer), in_file) != NULL ) {
    // discard any lines that return -1 ENOENT, as these are commands that failed
    if ( sscanf(buffer, "%d execve(\"%[^\n]\n", &pid, args) == 2  && strstr(args, "ENOENT") == NULL) {
      // current line matches the desired format, check whether the command is one of
      //  the desired commands: gcc, g++, ld, as

      // if previous line was a vfork, save the current pid and use it instead of the newly read in one
      if ( vfork ) {
        pid = saved_pid;
      }
      else {
        saved_pid = pid;
      }

      int command_end_index = 0; //the index of the " at the end of the filepath to the executed command
      //TODO: change to strchr
      for ( int i = 0; i < strlen(args); i++ ) {
        if ( args[i] == '\"' ) {
          break;
        }
        command_end_index++;
      }
      int command_start_index = 0; //the index of the first letter in the name of the command to be run
      for ( int i = command_end_index - 1; i >= 0; i-- ) {
        if ( args[i] == '/' ) {
          command_start_index = i + 1;
          break;
        }
      }

      int cmd_len = command_end_index - command_start_index;
      //TODO: strndup for next 2 lines
      char *cmd_name = malloc(cmd_len + 1);
      strncpy(cmd_name, args + command_start_index, cmd_len);
      //*(cmd_name + cmd_len + 1) = '\0'; //null terminator
      *(cmd_name + cmd_len) = '\0'; //null terminator

      if ( is_desired_cmd(cmd_name) == true) {
        if ( !strcmp(cmd_name, "gcc") || !strcmp(cmd_name, "g++") ) {
          LIST_add(fps_list, pid, cmd_name);
        }
        //parse the line and add appropriate entries in list of source files and list of commands
        char *source = extract_sources(args);
        if ( source != NULL ) {
          fprintf(sources_file, "%s/%s\n", pwd, source);
        }
        // the arguments passed to the executable run by execve are formated as such:
        //   ["arg1", "arg2", ..."argn"]
        int lbracket_index = -1;
        int rbracket_index = -1;
        for ( int i = 0; i < strlen(args); i++ ) {
          if ( args[i] == ']' ) {
            rbracket_index = i;
            break;
          }
          else if ( lbracket_index == -1 && args[i] == '[' ) {
            lbracket_index = i;
          }
        }
        char cmd_buffer[BUFFER_SIZE];
        if ( !strcmp(cmd_name, "gcc") || !strcmp(cmd_name, "g++") ) {
          //this is the start of a new target, need to output the old target to dependency file and
          // copy the dependencies to sandbox dir
          if ( cur_target != NULL ) {
            emit_target_to_file(dep_file, cur_target);
            TARGET_copy_deps(cur_target, sandbox_pwd);
            emit_target_to_makefile(sandbox_mkfile, sandbox_pwd, cur_target);
            //add the target to the list of make targets
            //TODO: add the target's name to the dependencies of all_make_targets
            //TARGET_add_dep(make_list, strdup(cur_target->target_name));
            //fprintf(stderr, "new make target\n");
            //fprintf(stderr, "MAKE_TARGET: \"%s\"\n", cur_target->target_name);
            strcat(make_targets_list, " ");
            strcat(make_targets_list, cur_target->target_name);
          }
          int i;
          int cmd_index = 0;
          for ( i = lbracket_index + 1; i < rbracket_index; i++ ) {
            cmd_buffer[i] = args[i];
            if ( args[i] != '\"' && args[i] != ',' ) {
              if ( args[i] != '\0' ) {
                fputc(args[i], cmds_file);
                cmd_buffer[cmd_index] = args[i];
                cmd_index++;
              }
            }
          }
          //TODO: free cur target's members here
          cur_target = malloc(sizeof(target));
          //parse the target file from the command
          cmd_buffer[i] = '\0';
          char *target_file = parse_target_from_cmd(cmd_buffer);
          cmd_buffer[cmd_index] = '\0'; //null terminate the command buffer
          cur_target->target_name = strndup(target_file, strlen(target_file));
          cur_target->cmd = strndup(cmd_buffer, strlen(cmd_buffer));

          // write newline in the commands file
          fputc('\n', cmds_file);
          if ( LIST_find_pid(fps_list, pid)  != NULL ) {
            TARGET_add_dep(cur_target, source);
          }
        } // end if ( gcc/g++ cmd match)
        else {
          //TODO: check if the cmd is as or ld
        }
      }
      free(cmd_name);
    } // end if (sscanf format match)
    else { // check for chdir calls, to change the current working directory appended to c/c++ file names
      char *new_cwd = strstr(buffer, "chdir(");
      if ( new_cwd != NULL ) { // syscall executed on this line was chdir, need to change cwd
        pwd = new_cwd + 7; // cut off \"chdir("\" from the beginning of new_cwd
        for ( int i = 0; i < strlen(pwd); i++ ) {
          if ( pwd[i] == '\"' ) {
            pwd[i] = '\0'; // null terminate the pathfile for the new working directory to cut off any further characters
            break;
          }
        }
      } // end if (chdir match)
      else {
        // check for openat
        char *openat = strstr(buffer, "openat(");
        //discard openat calls that return ENOENT, open failed
        if ( openat != NULL && strstr(openat, "ENOENT") == NULL &&
             ( LIST_find_pid(fps_list, pid) != NULL || strstr(openat, ".h") != NULL) ) {

          //ignore locale files being opened
          if ( strstr(openat, "locale") == NULL && strstr(openat, "/etc/") == NULL &&
               strstr(openat, "/types/") == NULL && strstr(openat, ".cache") == NULL &&
               strstr(openat, "/bits/") == NULL  && strstr(openat, "/tmp/") == NULL) {
            openat += 18; // cut off "openat(AT_FDCWD, \""
            for ( int i = 0; i < strlen(openat); i++ ) {
              if ( openat[i] == '\"' ) {
                openat[i] = '\0';
                break;
              }
            }
            TARGET_add_dep(cur_target, openat);
          }
        }
        else {
          //check for fork() calls
          if ( strstr(buffer, "vfork(") != NULL && strstr(buffer, "unfinished") != NULL ) {
            vfork = true;
          }
          else if ( strstr(buffer, "vfork resumed") != NULL ) {
            vfork = false;
          }
        } // end else (openat match)
      } //end else (chdir match)
    } // end else (sscanf match);
  } // end while

  //emit the last target
  if ( cur_target != NULL ) {
    emit_target_to_file(dep_file, cur_target);
    TARGET_copy_deps(cur_target, sandbox_pwd);
    emit_target_to_makefile(sandbox_mkfile, sandbox_pwd, cur_target);
    strcat(make_targets_list, " ");
    strcat(make_targets_list, cur_target->target_name);
  }

  //write the all_make_targets wrapper target at the end of the makefile
  fprintf(sandbox_mkfile, "\nall_make_targets:%s", make_targets_list);

  //print message detailing where to find sandbox directory
  fprintf(stdout, "\nThe generated sandbox directory can be found at %s\n", sandbox_pwd);
  fprintf(stdout, "In this directory, you may examine and modify the source files and their");
  fprintf(stdout, " dependencies and rebuild the tool\n");
  fprintf(stdout, "To build the sandboxed version of the tool, change directories to that");
  fprintf(stdout, " directory, and use the following command:\n\n\tmake\n\n");

  //close opened files
  fclose(in_file);
  fclose(cmds_file);
  fclose(sources_file);
  fclose(dep_file);
  fclose(sandbox_mkfile);
} // end main
