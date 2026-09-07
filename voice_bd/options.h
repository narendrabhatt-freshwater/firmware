/*

          Option Processing Macros

     Last Edited: 16:46:48   1 Feb  1989 - spr
     -----------------------------------------

16:46:54   1 Feb  1989 - spr: added optstropt macro

     Usage:                                                            
          [ opt_skip ]             ...use this to skip program name argv[0]
          opt_begin(tolower)       ...tolower folds upper->lower, else use 'null'
               option(a, opt_a)
               ...
               option(z, opt_z)
               [opt_valid]         ...exit with E$BADOPT;
OR             [opt_error(fn)]     ...call fn() with ptr to option char;
OR             default: <stmt>     ...invalid option statement
          opt_end
OR        opt_halt                 ...stop scanning on first non-option group
OR        opt_else(func)           ...call func() with ptr to non-option args

     To define an action to be processed for each option found (eg, to
     check for option conflicts etc), use, for example:

     #define _setopt(opt) action('opt')
*/

#define OSK
#define opt_skip ++argv;--argc;

#define opt_begin(fn) {char**v=argv-1;int c=argc;argc=0;while(c--){if((**++v=='-')&&((*v)[1]>'9'))while(*++*v)switch(fn(**v)){

// #define opt_begin(fn) {long atol(const char*),htol();double atof();register char**v=argv-1;register int c=argc;argc=0;while(c--){if((**++v=='-')&&((*v)[1]>'9'))while(*++*v)switch(fn(**v)){

/* optional: use one of these for unknown option processing */
#define opt_valid default:exit(181);
#define opt_error(fn) default:fn(&opt);

/* use one of these for non-option argument processing */
#define opt_end }else argv[argc++]=*v;endarg:;}}
#define opt_halt }else{do{argv[argc++]=*v++;}while(c--);break;}endarg:;}}
#define opt_else(fn) }else fn(*v,c,argv,argc);endarg:;}}

#define xopt (**v)              /* "opt" is current option character */
#define null(arg) arg         /* "null" is do nothing function */
#define _setopt(xopt)          /* optional pre-processing function */

#ifdef OSK
#define _optstr(p) ((*p)[1]=='='?((*p)[1]='\0',*p+2):"")
#endif

#define option(name,flag) case name:_setopt(name);(flag)=1;break;
#define codopt(name,code) case  name:_setopt(name);code;break;
#define stropt(name,var) case name:_setopt(name);var=_optstr(v);break;
#define optstropt(name,flag,var) case 'name':_setopt(name);(flag)+=1;var=_optstr(v);break;
#define intopt(name,var) case name:_setopt(name);var=atoi(_optstr(v));break;
#define hexopt(name,var) case name:_setopt(name);var=htoi(_optstr(v));break;
#define lhexopt(name,var) case name:_setopt(name);var=htol(_optstr(v));break;
#define longopt(name,var) case name:_setopt(name);var=atol(_optstr(v));break;
#define floatopt(name,var) case name:_setopt(name);var=atof(_optstr(v));break;
#define funopt(name,func) case name:_setopt(name);func(_optstr(v));break;
#define argopt(name) case name:_setopt(name);*--*v='-';argv[argc++]=*v;goto endarg;

/* end: option.h */
