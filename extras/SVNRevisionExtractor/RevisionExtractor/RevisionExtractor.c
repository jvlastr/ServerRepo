#define _CRT_SECURE_NO_WARNINGS 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

char * ASCENT_BRANCH = "-TRUNK";
int revision = 494;

int main(int argc, char* argv[])
{
	FILE * pFile = fopen("../../entries", "r");
	FILE * pTagFile = fopen("../../ascent-tag", "r");
	if( pTagFile != NULL )
	{
		char str[1024];
		fgets(str, 1024, pTagFile);
		ASCENT_BRANCH = (char*)malloc(strlen(str)+1);
		memcpy(ASCENT_BRANCH, str, strlen(str)+1);
		fclose( pTagFile );
	}
	printf("SVN Revision Extractor\n");
	printf("Originally written by Burlex"); 
	printf("Re-written by Dephon, 10/2/2013\n");
	printf("Branch: %s\n", ASCENT_BRANCH);
	printf("Ascent Revision %d", revision);

	if( pFile == NULL )
	{
		pFile = fopen( "svn_revision.h", "w" );
		if( pFile == NULL )
		{
			printf("Output file could not be opened.\n");
			return 2;
		}
		else
		{
			fprintf(pFile, "// This file was automatically generated by the SVN revision extractor.\n");
			fprintf(pFile, "// There is no need to modify it.\n");
			fprintf(pFile, "\n");
			fprintf(pFile, "#ifndef SVN_REVISION_H\n");
			fprintf(pFile, "#define SVN_REVISION_H\n\n");
			fprintf(pFile, "static const char * BUILD_TAG = \"%s\";\n", ASCENT_BRANCH);
			fprintf(pFile, "static int BUILD_REVISION = %d;\n\n", revision);
			fprintf(pFile, "#endif\t\t // SVN_REVISION_H\n\n");
			fflush( pFile );
			fclose( pFile );

			printf("Output file written. Exiting cleanly.\n");
		}
	}
	return 0;
}