#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void main(void) {
	char *fn_copy;
	char *saveptr;
  char *file_name = "hello";
	char *process_name = strtok_r(file_name, " ", &saveptr);			
}