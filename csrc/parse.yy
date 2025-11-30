%{
#include<stdio.h>
#include<stdlib.h>
#include<string.h>

#include"script.h"
int yylex();
%}

%union {
	unsigned number;
	char const* string;
}

/* literals */
%token <string> STRING RWDATA
%token <number> NUMBER 

/* keywords */
%token SEGMENTS SECTIONS ENTRY DATA RODATA

%type <number> at length 
%type <string> into

%start toplevel_list

%%

at : '@' NUMBER {
	$$ = $2;
} | { $$ = -1; };

length : { $$ = -1; } | '$' NUMBER {
	$$ = $2;
};

into : '>' STRING {
	$$ = $2;
};

segmentdefn : STRING RWDATA at length 
{
	add_script_segment($1, parse_segment_flags($2), $3, $4);
};

sectiondefn : STRING at into
{
	add_script_section($1, $3, $2);
};

segmentlist : /* empty */
            | segmentlist segmentdefn;

sectionlist : /* empty */
            | sectionlist sectiondefn;

segments : SEGMENTS '{' segmentlist '}';
sections : SECTIONS '{' sectionlist '}';

entry : ENTRY '(' STRING ',' STRING ')' {
	if(code_segment_name) 
		fprintf(stderr, "code segment redefined in linker script\n"), exit(1);

	code_segment_name = strdup($3);
	entry_symbol_name = strdup($5);
};

data : DATA '(' STRING ')' {
	if(data_segment_name) 
		fprintf(stderr, "data segment redefined in linker script\n"), exit(1);

	data_segment_name = strdup($3);
};

rodata : RODATA '(' STRING ')' {
	if(rodata_segment_name) 
		fprintf(stderr, "rodata segment redefined in linker script\n"), exit(1);

	rodata_segment_name = strdup($3);
};

toplevel : 
	  segments | sections 
	| entry | data | rodata;

toplevel_list : 
	| toplevel_list toplevel ;

%%
