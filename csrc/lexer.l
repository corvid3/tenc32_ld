%{
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include"script.h"
int fileno(FILE *stream);
extern char* strdup(char const* restrict in);
%}

%option noyywrap yylineno
%array

IDENTIFIER [[:alpha:]]+
HEXADEC 0x[[:xdigit:]]+
DECIMAL [[:digit:]]+

STRING      \"(\\.|[^"\\])*\"

%%

 /* defer the keyword parsing to an external function 
  * so that keywords may be case insensitive 
  */
{IDENTIFIER} { return parse_keyword(yytext); };
{HEXADEC} { yylval.number = parse_hex(yytext); return NUMBER; }
{DECIMAL} { yylval.number = parse_dec(yytext); return NUMBER; }
"["[rwx]+"]" { yytext[yyleng-1] = 0; yylval.string = strdup(yytext + 1); return RWDATA; }

{STRING} { yytext[yyleng-1] = 0; yylval.string = strdup(yytext + 1); return STRING; }

#.*\n ;
[ \t\n] ;

[{}()>$@,] { return yytext[0]; }

. { 
	char str[512];
	snprintf(str, sizeof str, "unknown character in linker script %c\n", *yytext);
	yyerror(str); 
}

%%
