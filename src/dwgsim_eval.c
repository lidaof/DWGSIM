#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <unistd.h>
#include <float.h>
#include <sys/resource.h>
#include <math.h>
#include "samtools/bam.h"
#include "samtools/sam.h"
#include "dwgsim_eval.h"

int 
print_usage()
{
  fprintf(stderr, "Usage: dwgsim_eval [options] <in.sam/in.bam>\n");
  fprintf(stderr, "\t-a\t\tsplit alignments by alignment score instead of mapping quality\n");
  fprintf(stderr, "\t-b\t\talignments are from BWA\n");
  fprintf(stderr, "\t-c\t\tcolor space alignments\n");
  fprintf(stderr, "\t-d\tINT\tdivide quality/alignment score by this factor\n");
  fprintf(stderr, "\t-e\tINT\tprint only alignments with the number of specified errors\n");
  fprintf(stderr, "\t-i\t\tprint only alignments with indels\n");
  fprintf(stderr, "\t-g\t\tgap \"wiggle\"\n");
  fprintf(stderr, "\t-n\tINT\tnumber of raw input paired-end reads (otherwise, inferred from all SAM records present).\n");
  fprintf(stderr, "\t-p\t\tprint incorrect alignments\n");
  fprintf(stderr, "\t-q\tINT\tconsider only alignments with this mapping quality or greater.\n");
  fprintf(stderr, "\t-z\t\tinput contains only single end reads\n");
  fprintf(stderr, "\t-s\tINT\tprint only alignments with the number of specified SNPs\n");
  fprintf(stderr, "\t-S\t\tinput is SAM\n");
  fprintf(stderr, "\t-h\t\tprints this help message\n");
  return 1;
}

/* Action */
enum {Exit, Warn, LastActionType};
/* Type */
enum {  
    Dummy,
    OutOfRange, /* e.g. command line args */
    InputArguments, 
    IllegalFileName,   
    IllegalPath,
    OpenFileError,
    EndOfFile,
    ReallocMemory,
    MallocMemory,
    ThreadError,
    ReadFileError,
    WriteFileError,
    DeleteFileError,
    LastErrorType,
};       
#define BREAK_LINE "************************************************************\n"
void
dwgsim_eval_print_error(char* FunctionName, char *VariableName, char* Message, int Action, int type)
{
  static char ErrorString[][20]=
    { "\0", "OutOfRange", "InputArguments", "IllegalFileName", "IllegalPath", "OpenFileError", "EndOfFile", "ReallocMemory", "MallocMemory", "ThreadError", "ReadFileError", "WriteFileError", "DeleteFileError"};
  static char ActionType[][20]={"Fatal Error", "Warning"};
  fprintf(stderr, "%s\rIn function \"%s\": %s[%s]. ",
          BREAK_LINE, FunctionName, ActionType[Action], ErrorString[type]);

  /* Only print variable name if is available */
  if(VariableName) {
      fprintf(stderr, "Variable/Value: %s.\n", VariableName);
  }
  /* Only print message name if is available */
  if(Message) {
      fprintf(stderr, "Message: %s.\n", Message);
  }
  if(type == ReadFileError ||
     type == OpenFileError ||
     type == WriteFileError) {
      perror("The file stream error was:");
  }

  switch(Action) {
    case Exit:
      fprintf(stderr, " ***** Exiting due to errors *****\n");
      fprintf(stderr, "%s", BREAK_LINE);
      exit(EXIT_FAILURE);
      break; /* Not necessary actually! */
    case Warn:
      fprintf(stderr, " ***** Warning *****\n");
      fprintf(stderr, "%s", BREAK_LINE);
      break;
    default:
      fprintf(stderr, "Trouble!!!\n");
      fprintf(stderr, "%s", BREAK_LINE);
  }
}


int 
main(int argc, char *argv[])
{
  char c;
  dwgsim_eval_args_t args;

  args.a = args.b = args.c = args.i = args.n = args.p = args.q = args.z = 0; 
  args.d = 1;
  args.e = -1;
  args.g = 5;
  args.s = -1;
  args.S = 0;

  while(0 <= (c = getopt(argc, argv, "d:e:g:n:q:s:abchipzS"))) {
      switch(c) {
        case 'a': args.a = 1; break;
        case 'b': args.b = 1; break;
        case 'c': args.c = 1; break;
        case 'd': args.d = atoi(optarg); break;
        case 'e': args.e = atoi(optarg); break;
        case 'g': args.g = atoi(optarg); break;
        case 'h': return print_usage(); break;
        case 'i': args.i = 1; break;
        case 'n': args.n = atoi(optarg); break;
        case 'p': args.p = 1; break;
        case 'q': args.q = atoi(optarg); break;
        case 's': args.s = atoi(optarg); break;
        case 'z': args.z = 1; break;
        case 'S': args.S = 1; break;
        default: fprintf(stderr, "Unrecognized option: -%c\n", c); return 1;
      }
  }

  if(argc == optind) {
      return print_usage();
  }

  run(&args, argc - optind, argv + optind);

  return 0;
}

void 
run(dwgsim_eval_args_t *args,
    int32_t num_files,
    char *files[]) 
{
  char *FnName="run";
  int32_t i, n = 0;
  samfile_t *fp_in = NULL;
  samfile_t *fp_out = NULL;
  bam1_t *b=NULL;
  dwgsim_eval_counts_t *counts;
  char *prev_qname=NULL;

  // initialize counts
  counts = dwgsim_eval_counts_init();

  fprintf(stderr, "Analyzing...\nCurrently on:\n0");
  for(i=0;i<num_files;i++) {
      // Open the file
      fp_in = samopen(files[i], (1 == args->S) ? "r" : "rb", 0); 
      if(NULL == fp_in) {
          dwgsim_eval_print_error(FnName, files[i], "Could not open file for reading", Exit, OpenFileError);
      }

      if(0 == i && 1 == args->p) {
          fp_out = samopen("-", "w", fp_in->header);
          if(NULL == fp_out) {
              dwgsim_eval_print_error(FnName, "stdout", "Could not open file stream for writing", Exit, OpenFileError);
          }
      }

      b = bam_init1();
      while(0 < samread(fp_in, b)) {
          if(NULL == prev_qname ||
             0 != strcmp(prev_qname, bam1_qname(b))) {

              process_bam(counts, args, fp_in->header, b, fp_out);

              if((BAM_FPAIRED & b->core.flag)) { // paired end
                  if(1 == args->z) { // expect single end
                      dwgsim_eval_print_error(FnName, NULL, "Found a read that was paired end", Exit, OutOfRange);
                  }
                  if((BAM_FREAD1 & b->core.flag)) { // count # of pairs
                      n++;
                  }
              }
              else { // single end
                  if(0 == args->z) { // expect paired end
                      dwgsim_eval_print_error(FnName, NULL, "Found a read that was not paired", Exit, OutOfRange);
                  }
                  n++;
              }

              free(prev_qname);
              prev_qname = strdup(bam1_qname(b));

              if(0 == (n % 10000)) {
                  fprintf(stderr, "\r%lld", (long long int)n);
              }
          }

          bam_destroy1(b);
          b = bam_init1();
      }
      bam_destroy1(b);

      // Close the file
      samclose(fp_in);
  }
  free(prev_qname);

  if(1 == args->p) samclose(fp_out);

  fprintf(stderr, "\r%lld\n", (long long int)n);

  if(0 < args->n) {
      if(n != args->n) {
          fprintf(stderr, "(-n)=%d\tn=%d\n", args->n, n);
          dwgsim_eval_print_error(FnName, NULL, "Number of reads found differs from the number specified (-n)", Warn, OutOfRange);
      }
  }
  if(0 == args->z) {
      dwgsim_eval_counts_print(counts, args->a, args->d, (0 < args->n) ? 2*args->n : 2*n);
  }
  else {
      dwgsim_eval_counts_print(counts, args->a, args->d, (0 < args->n) ? args->n : n);
  }

  dwgsim_eval_counts_destroy(counts);

  fprintf(stderr, "Analysis complete.\n");
}


void
process_bam(dwgsim_eval_counts_t *counts,
            dwgsim_eval_args_t *args,
            bam_header_t *header,
            bam1_t *b,
            samfile_t *fp_out)
{
  char *FnName="process_bam";
  int32_t qual=INT_MIN, alignment_score=INT_MIN, left;
  char *chr=NULL;
  char *name=NULL;
  char chr_name[1028]="\0";
  char read_num[1028]="\0";
  int32_t pos_1, pos_2, str_1, str_2, rand_1, rand2; 
  int32_t n_err_1, n_sub_1, n_indel_1, n_err_2, n_sub_2, n_indel_2;
  int32_t pos, str, rand;
  int32_t n_err, n_sub, n_indel;
  int32_t i, j, tmp;
  int32_t predicted_value, actual_value;

  // mapping quality threshold
  if(b->core.qual < args->q) return;

  // parse read name
  name = strdup(bam1_qname(b));
  char *to_rm="_::_::_______"; // to remove
  for(i=b->core.l_qname-1,j=0;0<=i && j<13;i--) { // replace with spaces 
      if(name[i] == to_rm[j]) {
          name[i] = ' '; j++; 
      }
  }
  if(14 != sscanf(name, "%s %d %d %1d %1d %1d %1d %d %d %d %d %d %d %s",
                  chr_name, &pos_1, &pos_2, &str_1, &str_2, &rand_1, &rand2,
                  &n_err_1, &n_sub_1, &n_indel_1,
                  &n_err_2, &n_sub_2, &n_indel_2,
                  read_num)) {
      dwgsim_eval_print_error(FnName, name, "[dwgsim_eval] read was not generated by dwgsim?", Warn, OutOfRange);
      free(name);
      return;
  }
  free(name);

  // get metric value
  if(0 == args->a) {
      qual = (b->core.qual / args->d); 
      if(DWGSIM_EVAL_MAXQ < qual) qual = DWGSIM_EVAL_MAXQ;
  }
  else {
      uint8_t *aux = bam_aux_get(b, "AS");
      if(NULL != aux) {
          alignment_score = (bam_aux2i(aux) / args->d);
          if(alignment_score < DWGSIM_EVAL_MINAS) alignment_score = DWGSIM_EVAL_MINAS;
      }
      else { // no alignment score present
          alignment_score = DWGSIM_EVAL_MINAS;
      }
  }

  if(1 == args->i) { // indels only
      if(1 == args->z || (b->core.flag & BAM_FREAD1)) {
          if(0 == n_indel_1) return;
      }
      else {
          if(0 == n_indel_2) return;
      }
  }
  else if(0 <= args->e && n_err_1 !=  args->e) { // # of errors
      return;
  }
  else if(0 <= args->s && n_sub_1 !=  args->s) { // # of snps
      return;
  }

  if(1 == args->c && 1 == args->b) { // SOLiD and BWA
      // Swap 1 and 2
      tmp=n_err_1; n_err_1=n_err_2; n_err_2=tmp;
      tmp=n_sub_1; n_sub_1=n_sub_2; n_sub_2=tmp;
      tmp=n_indel_1; n_indel_1=n_indel_2; n_indel_2=tmp;
  }

  // copy data
  if(1 == args->z || (b->core.flag & BAM_FREAD1)) {
      pos = pos_1; str = str_1; rand = rand_1;
      n_err = n_err_1; n_sub = n_sub_1; n_indel = n_indel_1;
  }
  else {
      pos = pos_2; str = str_2; rand = rand2;
      n_err = n_err_2; n_sub = n_sub_2; n_indel = n_indel_2;
  }

  // get the actual value 
  if(1 == rand) {
      actual_value = DWGSIM_EVAL_UNMAPPABLE;
  }
  else {
      actual_value = DWGSIM_EVAL_MAPPABLE;
  }

  // get the predicted value
  if((BAM_FUNMAP & b->core.flag)) { // unmapped
      predicted_value = DWGSIM_EVAL_UNMAPPED;
  }
  else { // mapped (correctly?)
      chr = header->target_name[b->core.tid];
      left = b->core.pos;

      if(1 == rand || // should not map 
         0 != strcmp(chr, chr_name)  // different chromosome
         || args->g < fabs(pos - left)) { // out of bounds (positionally) 
          predicted_value = DWGSIM_EVAL_MAPPED_INCORRECTLY;
      }
      else {
          predicted_value = DWGSIM_EVAL_MAPPED_CORRECTLY;
      }
  }

  dwgsim_eval_counts_add(counts, (0 == args->a) ? qual : alignment_score, actual_value, predicted_value);

  // print incorrect alignments
  if(1 == args->p && DWGSIM_EVAL_MAPPED_INCORRECTLY == predicted_value) {
      if(samwrite(fp_out, b) <= 0) {
          dwgsim_eval_print_error(FnName, "stdout", "Could not write to stream", Exit, WriteFileError);
      }
  }
}

dwgsim_eval_counts_t *
dwgsim_eval_counts_init()
{
  dwgsim_eval_counts_t *counts;

  counts = malloc(sizeof(dwgsim_eval_counts_t));

  counts->min_score = counts->max_score = 0;

  counts->mc = malloc(sizeof(int32_t)); assert(NULL != counts->mc);
  counts->mi = malloc(sizeof(int32_t)); assert(NULL != counts->mi);
  counts->mu = malloc(sizeof(int32_t)); assert(NULL != counts->mu);
  counts->um = malloc(sizeof(int32_t)); assert(NULL != counts->um);
  counts->uu = malloc(sizeof(int32_t)); assert(NULL != counts->uu);

  counts->mc[0] = counts->mi[0] = counts->mu[0] = 0;
  counts->um[0] = counts->uu[0] = 0;

  return counts;
}

void
dwgsim_eval_counts_destroy(dwgsim_eval_counts_t *counts)
{
  free(counts->mc);
  free(counts->mi);
  free(counts->mu);
  free(counts->um);
  free(counts->uu);
  free(counts);
}

void 
dwgsim_eval_counts_add(dwgsim_eval_counts_t *counts, int32_t score, int32_t actual_value, int32_t predicted_value)
{
  char *FnName="dwgsim_eval_counts_add";
  int32_t i, m, n;
  if(counts->max_score < score) {
      m = score - counts->min_score + 1;
      n = counts->max_score - counts->min_score + 1;

      counts->mc = realloc(counts->mc, sizeof(int32_t)*m); assert(NULL != counts->mc);
      counts->mi = realloc(counts->mi, sizeof(int32_t)*m); assert(NULL != counts->mi);
      counts->mu = realloc(counts->mu, sizeof(int32_t)*m); assert(NULL != counts->mu);
      counts->um = realloc(counts->um, sizeof(int32_t)*m); assert(NULL != counts->um);
      counts->uu = realloc(counts->uu, sizeof(int32_t)*m); assert(NULL != counts->uu);

      // initialize to zero
      for(i=n;i<m;i++) {
          counts->mc[i] = counts->mi[i] = counts->mu[i] = 0;
          counts->um[i] = counts->uu[i] = 0;
      }
      counts->max_score = score;
  }
  else if(score < counts->min_score) {
      m = counts->max_score - score + 1;
      n = counts->max_score - counts->min_score + 1;

      counts->mc = realloc(counts->mc, sizeof(int32_t)*m); assert(NULL != counts->mc);
      counts->mi = realloc(counts->mi, sizeof(int32_t)*m); assert(NULL != counts->mi);
      counts->mu = realloc(counts->mu, sizeof(int32_t)*m); assert(NULL != counts->mu);
      counts->um = realloc(counts->um, sizeof(int32_t)*m); assert(NULL != counts->um);
      counts->uu = realloc(counts->uu, sizeof(int32_t)*m); assert(NULL != counts->uu);

      // shift up
      for(i=m-1;m-n<=i;i--) {
          counts->mc[i] = counts->mc[i-(m-n)]; 
          counts->mi[i] = counts->mi[i-(m-n)]; 
          counts->mu[i] = counts->mu[i-(m-n)]; 
          counts->um[i] = counts->um[i-(m-n)]; 
          counts->uu[i] = counts->uu[i-(m-n)]; 
      }
      // initialize to zero
      for(i=0;i<m-n;i++) {
          counts->mc[i] = counts->mi[i] = counts->mu[i] = 0;
          counts->um[i] = counts->uu[i] = 0;
      }
      counts->min_score = score;
  }

  // check actual value
  switch(actual_value) {
    case DWGSIM_EVAL_MAPPABLE:
    case DWGSIM_EVAL_UNMAPPABLE:
      break;
    default:
      dwgsim_eval_print_error(FnName, "actual_value", "Could not understand actual value", Exit, OutOfRange);
  }

  // check predicted value
  switch(predicted_value) {
    case DWGSIM_EVAL_MAPPED_CORRECTLY:
    case DWGSIM_EVAL_MAPPED_INCORRECTLY:
    case DWGSIM_EVAL_UNMAPPED:
      break;
    default:
      dwgsim_eval_print_error(FnName, "predicted_value", "Could not understand predicted value", Exit, OutOfRange);
  }

  switch(actual_value) {
    case DWGSIM_EVAL_MAPPABLE:
      switch(predicted_value) {
        case DWGSIM_EVAL_MAPPED_CORRECTLY:
          counts->mc[score-counts->min_score]++; break;
        case DWGSIM_EVAL_MAPPED_INCORRECTLY:
          counts->mi[score-counts->min_score]++; break;
        case DWGSIM_EVAL_UNMAPPED:
          counts->mu[score-counts->min_score]++; break;
        default:
          break; // should not reach here
      }
      break;
    case DWGSIM_EVAL_UNMAPPABLE:
      switch(predicted_value) {
        case DWGSIM_EVAL_MAPPED_CORRECTLY:
          dwgsim_eval_print_error(FnName, "predicted_value", "predicted value cannot be mapped correctly when the read is unmappable", Exit, OutOfRange); break;
        case DWGSIM_EVAL_MAPPED_INCORRECTLY:
          counts->um[score-counts->min_score]++; break;
        case DWGSIM_EVAL_UNMAPPED:
          counts->uu[score-counts->min_score]++; break;
        default:
          break; // should not reach here
      }
      break;
    default:
      break; // should not reach here
  }
}

void 
dwgsim_eval_counts_print(dwgsim_eval_counts_t *counts, int32_t a, int32_t d, int32_t n)
{
  int32_t i;
  int32_t max = 0;
  int32_t mc_sum, mi_sum, mu_sum, um_sum, uu_sum;
  int32_t m_total, mm_total, u_total;
  char format[1024]="\0";

  mc_sum = mi_sum = mu_sum = um_sum = uu_sum = 0;
  m_total = mm_total = u_total = 0;

  // create the format string
  for(i=counts->max_score - counts->min_score;0<=i;i--) {
      m_total += counts->mc[i] + counts->mi[i] + counts->mu[i];
      u_total += counts->um[i] + counts->uu[i];
      max += counts->mc[i] + counts->mi[i] + counts->mu[i] + counts->um[i] + counts->uu[i];
  }
  max = 1 + log10(max);
  strcat(format, "%.2d ");
  for(i=0;i<12;i++) {
      sprintf(format + (int)strlen(format), "%%%dd ", max);
  }
  strcat(format + (int)strlen(format), "%.3e %.3e %.3e %.3e %.3e %.3e\n");

  // header
  fprintf(stderr, "# thr | the minimum %s threshold\n", (0 == a) ? "mapping quality" : "alignment score");
  fprintf(stderr, "# mc | the number of reads mapped correctly that should be mapped at the threshold\n");
  fprintf(stderr, "# mi | the number of reads mapped incorrectly that should be mapped be mapped at the threshold\n");
          
  fprintf(stderr, "# mu | the number of reads unmapped that should be mapped be mapped at the threshold\n");
          
  fprintf(stderr, "# um | the number of reads mapped that should be unmapped be mapped at the threshold\n");
  fprintf(stderr, "# uu | the number of reads unmapped that should be unmapped be mapped at the threshold\n");
  fprintf(stderr, "# mc' + mi' + mu' + um' + uu' | the total number of reads mapped at the threshold\n");
  fprintf(stderr, "# mc' | the number of reads mapped correctly that should be mapped at or greater than that threshold\n");
  fprintf(stderr, "# mi' | the number of reads mapped incorrectly that should be mapped be mapped at or greater than that threshold\n");
          
  fprintf(stderr, "# mu' | the number of reads unmapped that should be mapped be mapped at or greater than that threshold\n");
          
  fprintf(stderr, "# um' | the number of reads mapped that should be unmapped be mapped at or greater than that threshold\n");
  fprintf(stderr, "# uu' | the number of reads unmapped that should be unmapped be mapped at or greater than that threshold\n");
  fprintf(stderr, "# mc' + mi' + mu' + um' + uu' | the total number of reads mapped at or greater than the threshold\n");
          
  fprintf(stderr, "# (mc / (mc' + mi' + mu')) | sensitivity: the fraction of reads that should be mapped that are mapped correctly at the threshold\n");
  fprintf(stderr, "# (mc / mc' + mi') | positive predictive value: the fraction of mapped reads that are mapped correctly at the threshold\n");
  fprintf(stderr, "# (um / (um' + uu')) | false discovery rate: the fraction of random reads that are mapped at the threshold\n");
  fprintf(stderr, "# (mc' / (mc' + mi' + mu')) | sensitivity: the fraction of reads that should be mapped that are mapped correctly at or greater than the threshold\n");
  fprintf(stderr, "# (mc' / mc' + mi') | positive predictive value: the fraction of mapped reads that are mapped correctly at or greater than the threshold\n");
  fprintf(stderr, "# (um' / (um' + uu')) | false discovery rate: the fraction of random reads that are mapped at or greater than the threshold\n");


  // print
  for(i=counts->max_score - counts->min_score;0<=i;i--) {
      double num, den;

      mc_sum += counts->mc[i];
      mi_sum += counts->mi[i];
      mu_sum += counts->mu[i];
      um_sum += counts->um[i];
      uu_sum += counts->uu[i];
      mm_total += counts->mc[i] + counts->mi[i];

      /* Notes:
       *  notice that the denominator for sensitivity (and fdr) for the "ge" 
       *  (greater than or equal) threshold is the "total", while the denominator 
       *  for ppv is "@ >= Q".  The reasoning behind this is ppv is a measure of
       *  the quality of mappings that will be returned when using a Q threshold
       *  while the sensitivity want to measure the fraction of mappings that
       *  will be returned compared to the maximum.  Basically, if we accept
       *  only mappings at a given threshold, and call the rest unmapped, what
       *  happens?
       *  - sensitivity tells us the # of correct mappings out of the total possible
       *  mappings.
       *  - ppv tells us the # of correct mappings out of the total mappings.
       *  - fdr tells us the # of random mappings out of the total unmappable.
       */
       
      // "at" sensitivity: mapped correctly @ Q / mappable @ Q
      num = counts->mc[i];
      den = counts->mc[i] + counts->mi[i] + counts->mu[i];
      double sens_at_thr = (0 == den) ? 0. : (num / (double)den);  
      // "ge" sensitivity: mapped correctly @ >= Q / total mappable
      double sens_ge_thr = (0 == m_total) ? 0. : (mc_sum / (double)m_total);
      
      // "at" positive predictive value: mapped correctly @ Q / mappable and mapped @ Q
      num = counts->mc[i];
      den = counts->mc[i] + counts->mi[i];
      double ppv_at_thr = (0 == den) ? 0. : (num / (double)den);
      // "ge" positive predictive value: mapped correctly @ >= Q / mappable and mapped @ >= Q
      double ppv_ge_thr = (0 == mm_total) ? 0. : (mc_sum / (double)mm_total);

      // "at" false discovery rate: unmappable and mapped @ Q / unmappable @ Q
      num = counts->um[i];
      den = counts->um[i] + counts->uu[i];
      double fdr_at_thr = (0 == den) ? 0. : (num / (double)den);
      // "ge" false discovery rate: unmappable and mapped @ >= Q / unmappable @ >= Q
      double fdr_ge_thr = (0 == u_total) ? 0. : (um_sum / (double)u_total);
      
      fprintf(stdout, format,
              (i + counts->min_score)*d,
              counts->mc[i], counts->mi[i], counts->mu[i], counts->um[i], counts->uu[i],
              counts->mc[i] + counts->mi[i] + counts->mu[i] + counts->um[i] + counts->uu[i],
              mc_sum, mi_sum, mu_sum, um_sum, uu_sum,
              mc_sum + mi_sum + mu_sum + um_sum + uu_sum,
              sens_at_thr, ppv_at_thr, fdr_at_thr,
              sens_ge_thr, ppv_ge_thr, fdr_ge_thr);
  }
}