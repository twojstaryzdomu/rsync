/* 
   Copyright (C) Andrew Tridgell 1996
   Copyright (C) Paul Mackerras 1996
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "rsync.h"

extern int csum_length;

extern int verbose;
extern int am_server;
extern int always_checksum;
extern time_t starttime;

extern int remote_version;

extern char *backup_suffix;

extern int block_size;
extern int update_only;
extern int make_backups;
extern int preserve_links;
extern int preserve_hard_links;
extern int preserve_perms;
extern int preserve_devices;
extern int preserve_uid;
extern int preserve_gid;
extern int preserve_times;
extern int dry_run;
extern int ignore_times;
extern int recurse;
extern int delete_mode;
extern int cvs_exclude;

/*
  free a sums struct
  */
static void free_sums(struct sum_struct *s)
{
  if (s->sums) free(s->sums);
  free(s);
}



/*
  send a sums struct down a fd
  */
static void send_sums(struct sum_struct *s,int f_out)
{
  int i;

  /* tell the other guy how many we are going to be doing and how many
     bytes there are in the last chunk */
  write_int(f_out,s?s->count:0);
  write_int(f_out,s?s->n:block_size);
  write_int(f_out,s?s->remainder:0);
  if (s)
    for (i=0;i<s->count;i++) {
      write_int(f_out,s->sums[i].sum1);
      write_buf(f_out,s->sums[i].sum2,csum_length);
    }
  write_flush(f_out);
}


/*
  generate a stream of signatures/checksums that describe a buffer

  generate approximately one checksum every n bytes
  */
static struct sum_struct *generate_sums(struct map_struct *buf,off_t len,int n)
{
  int i;
  struct sum_struct *s;
  int count;
  int block_len = n;
  int remainder = (len%block_len);
  off_t offset = 0;

  count = (len+(block_len-1))/block_len;

  s = (struct sum_struct *)malloc(sizeof(*s));
  if (!s) out_of_memory("generate_sums");

  s->count = count;
  s->remainder = remainder;
  s->n = n;
  s->flength = len;

  if (count==0) {
    s->sums = NULL;
    return s;
  }

  if (verbose > 3)
    fprintf(FERROR,"count=%d rem=%d n=%d flength=%d\n",
	    s->count,s->remainder,s->n,(int)s->flength);

  s->sums = (struct sum_buf *)malloc(sizeof(s->sums[0])*s->count);
  if (!s->sums) out_of_memory("generate_sums");
  
  for (i=0;i<count;i++) {
    int n1 = MIN(len,n);
    char *map = map_ptr(buf,offset,n1);

    s->sums[i].sum1 = get_checksum1(map,n1);
    get_checksum2(map,n1,s->sums[i].sum2);

    s->sums[i].offset = offset;
    s->sums[i].len = n1;
    s->sums[i].i = i;

    if (verbose > 3)
      fprintf(FERROR,"chunk[%d] offset=%d len=%d sum1=%08x\n",
	      i,(int)s->sums[i].offset,s->sums[i].len,s->sums[i].sum1);

    len -= n1;
    offset += n1;
  }

  return s;
}


/*
  receive the checksums for a buffer
  */
static struct sum_struct *receive_sums(int f)
{
  struct sum_struct *s;
  int i;
  off_t offset = 0;
  int block_len;

  s = (struct sum_struct *)malloc(sizeof(*s));
  if (!s) out_of_memory("receive_sums");

  s->count = read_int(f);
  s->n = read_int(f);
  s->remainder = read_int(f);  
  s->sums = NULL;

  if (verbose > 3)
    fprintf(FERROR,"count=%d n=%d rem=%d\n",
	    s->count,s->n,s->remainder);

  block_len = s->n;

  if (s->count == 0) 
    return(s);

  s->sums = (struct sum_buf *)malloc(sizeof(s->sums[0])*s->count);
  if (!s->sums) out_of_memory("receive_sums");

  for (i=0;i<s->count;i++) {
    s->sums[i].sum1 = read_int(f);
    read_buf(f,s->sums[i].sum2,csum_length);

    s->sums[i].offset = offset;
    s->sums[i].i = i;

    if (i == s->count-1 && s->remainder != 0) {
      s->sums[i].len = s->remainder;
    } else {
      s->sums[i].len = s->n;
    }
    offset += s->sums[i].len;

    if (verbose > 3)
      fprintf(FERROR,"chunk[%d] len=%d offset=%d sum1=%08x\n",
	      i,s->sums[i].len,(int)s->sums[i].offset,s->sums[i].sum1);
  }

  s->flength = offset;

  return s;
}


static void set_perms(char *fname,struct file_struct *file,struct stat *st,
		      int report)
{
  int updated = 0;
  struct stat st2;

  if (dry_run) return;

  if (!st) {
    if (stat(fname,&st2) != 0) {
      fprintf(FERROR,"stat %s : %s\n",fname,strerror(errno));
      return;
    }
    st = &st2;
  }

  if (preserve_times && !S_ISLNK(st->st_mode) &&
      st->st_mtime != file->modtime) {
    updated = 1;
    if (set_modtime(fname,file->modtime) != 0) {
      fprintf(FERROR,"failed to set times on %s : %s\n",
	      fname,strerror(errno));
      return;
    }
  }

#ifdef HAVE_CHMOD
  if (preserve_perms && !S_ISLNK(st->st_mode) &&
      st->st_mode != file->mode) {
    updated = 1;
    if (chmod(fname,file->mode) != 0) {
      fprintf(FERROR,"failed to set permissions on %s : %s\n",
	      fname,strerror(errno));
      return;
    }
  }
#endif

  if ((preserve_uid && st->st_uid != file->uid) || 
      (preserve_gid && st->st_gid != file->gid)) {
    updated = 1;
    if (chown(fname,
	      preserve_uid?file->uid:-1,
	      preserve_gid?file->gid:-1) != 0) {
      if (verbose>1 || preserve_uid)
	fprintf(FERROR,"chown %s : %s\n",fname,strerror(errno));
      return;
    }
  }
    
  if (verbose > 1 && report) {
    if (updated)
      fprintf(FINFO,"%s\n",fname);
    else
      fprintf(FINFO,"%s is uptodate\n",fname);
  }
}


void recv_generator(char *fname,struct file_list *flist,int i,int f_out)
{  
  int fd;
  struct stat st;
  struct map_struct *buf;
  struct sum_struct *s;
  char sum[SUM_LENGTH];
  int statret;
  struct file_struct *file = &flist->files[i];

  if (verbose > 2)
    fprintf(FERROR,"recv_generator(%s)\n",fname);

  statret = lstat(fname,&st);

#if SUPPORT_LINKS
  if (preserve_links && S_ISLNK(file->mode)) {
    char lnk[MAXPATHLEN];
    int l;
    if (statret == 0) {
      l = readlink(fname,lnk,MAXPATHLEN-1);
      if (l > 0) {
	lnk[l] = 0;
	if (strcmp(lnk,file->link) == 0) {
	  set_perms(fname,file,&st,1);
	  return;
	}
      }
    }
    if (!dry_run) unlink(fname);
    if (!dry_run && symlink(file->link,fname) != 0) {
      fprintf(FERROR,"link %s -> %s : %s\n",
	      fname,file->link,strerror(errno));
    } else {
      set_perms(fname,file,NULL,0);
      if (verbose) 
	fprintf(FINFO,"%s -> %s\n",
		fname,file->link);
    }
    return;
  }
#endif

#ifdef HAVE_MKNOD
  if (preserve_devices && IS_DEVICE(file->mode)) {
    if (statret != 0 || 
	st.st_mode != file->mode ||
	st.st_rdev != file->rdev) {	
      if (!dry_run) unlink(fname);
      if (verbose > 2)
	fprintf(FERROR,"mknod(%s,0%o,0x%x)\n",
		fname,(int)file->mode,(int)file->rdev);
      if (!dry_run && 
	  mknod(fname,file->mode,file->rdev) != 0) {
	fprintf(FERROR,"mknod %s : %s\n",fname,strerror(errno));
      } else {
	set_perms(fname,file,NULL,0);
	if (verbose)
	  fprintf(FINFO,"%s\n",fname);
      }
    } else {
      set_perms(fname,file,&st,1);
    }
    return;
  }
#endif

  if (preserve_hard_links && check_hard_link(file)) {
    if (verbose > 1)
      fprintf(FINFO,"%s is a hard link\n",file->name);
    return;
  }

  if (!S_ISREG(file->mode)) {
    fprintf(FERROR,"skipping non-regular file %s\n",fname);
    return;
  }

  if (statret == -1) {
    if (errno == ENOENT) {
      write_int(f_out,i);
      if (!dry_run) send_sums(NULL,f_out);
    } else {
      if (verbose > 1)
	fprintf(FERROR,"recv_generator failed to open %s\n",fname);
    }
    return;
  }

  if (!S_ISREG(st.st_mode)) {
    fprintf(FERROR,"%s : not a regular file\n",fname);
    return;
  }

  if (update_only && st.st_mtime >= file->modtime) {
    if (verbose > 1)
      fprintf(FERROR,"%s is newer\n",fname);
    return;
  }

  if (always_checksum && S_ISREG(st.st_mode)) {
    file_checksum(fname,sum,st.st_size);
  }

  if (st.st_size == file->length &&
      ((!ignore_times && st.st_mtime == file->modtime) ||
       (always_checksum && S_ISREG(st.st_mode) && 	  
	memcmp(sum,file->sum,csum_length) == 0))) {
    set_perms(fname,file,&st,1);
    return;
  }

  if (dry_run) {
    write_int(f_out,i);
    return;
  }

  /* open the file */  
  fd = open(fname,O_RDONLY);

  if (fd == -1) {
    fprintf(FERROR,"failed to open %s : %s\n",fname,strerror(errno));
    return;
  }

  if (st.st_size > 0) {
    buf = map_file(fd,st.st_size);
  } else {
    buf = NULL;
  }

  if (verbose > 3)
    fprintf(FERROR,"mapped %s of size %d\n",fname,(int)st.st_size);

  s = generate_sums(buf,st.st_size,block_size);

  write_int(f_out,i);
  send_sums(s,f_out);
  write_flush(f_out);

  close(fd);
  unmap_file(buf);

  free_sums(s);
}



static void receive_data(int f_in,struct map_struct *buf,int fd,char *fname)
{
  int i,n,remainder,len,count;
  off_t offset = 0;
  off_t offset2;
  char *data;

  count = read_int(f_in);
  n = read_int(f_in);
  remainder = read_int(f_in);

  for (i=recv_token(f_in,&data); i != 0; i=recv_token(f_in,&data)) {
    if (i > 0) {
      if (verbose > 3)
	fprintf(FERROR,"data recv %d at %d\n",i,(int)offset);

      if (write_sparse(fd,data,i) != i) {
	fprintf(FERROR,"write failed on %s : %s\n",fname,strerror(errno));
	exit_cleanup(1);
      }
      offset += i;
    } else {
      i = -(i+1);
      offset2 = i*n;
      len = n;
      if (i == count-1 && remainder != 0)
	len = remainder;

      if (verbose > 3)
	fprintf(FERROR,"chunk[%d] of size %d at %d offset=%d\n",
		i,len,(int)offset2,(int)offset);

      if (write_sparse(fd,map_ptr(buf,offset2,len),len) != len) {
	fprintf(FERROR,"write failed on %s : %s\n",fname,strerror(errno));
	exit_cleanup(1);
      }
      offset += len;
    }
  }

  if (offset > 0 && sparse_end(fd) != 0) {
    fprintf(FERROR,"write failed on %s : %s\n",fname,strerror(errno));
    exit_cleanup(1);
  }
}


static void delete_one(struct file_struct *f)
{
  if (!S_ISDIR(f->mode)) {
    if (!dry_run && unlink(f->name) != 0) {
      fprintf(FERROR,"unlink %s : %s\n",f->name,strerror(errno));
    } else if (verbose) {
      fprintf(FERROR,"deleting %s\n",f->name);
    }
  } else {    
    if (!dry_run && rmdir(f->name) != 0) {
      if (errno != ENOTEMPTY)
	fprintf(FERROR,"rmdir %s : %s\n",f->name,strerror(errno));
    } else if (verbose) {
      fprintf(FERROR,"deleting directory %s\n",f->name);      
    }
  }
}


static void delete_files(struct file_list *flist)
{
  struct file_list *local_file_list;
  char *dot=".";
  int i;

  if (cvs_exclude)
    add_cvs_excludes();

  if (!(local_file_list = send_file_list(-1,recurse,1,&dot)))
    return;

  for (i=local_file_list->count;i>=0;i--) {
    if (!local_file_list->files[i].name) continue;
    if (-1 == flist_find(flist,&local_file_list->files[i])) {
      delete_one(&local_file_list->files[i]);
    }    
  }
}

static char *cleanup_fname = NULL;

void exit_cleanup(int code)
{
  if (cleanup_fname)
    unlink(cleanup_fname);
  exit(code);
}

void sig_int(void)
{
  exit_cleanup(1);
}


int recv_files(int f_in,struct file_list *flist,char *local_name,int f_gen)
{  
  int fd1,fd2;
  struct stat st;
  char *fname;
  char fnametmp[MAXPATHLEN];
  struct map_struct *buf;
  int i;
  struct file_struct *file;
  int phase=0;

  if (verbose > 2) {
    fprintf(FERROR,"recv_files(%d) starting\n",flist->count);
  }

  if (recurse && delete_mode && !local_name && flist->count>0) {
    delete_files(flist);
  }

  while (1) 
    {
      i = read_int(f_in);
      if (i == -1) {
	if (phase==0 && remote_version >= 13) {
	  phase++;
	  write_int(f_gen,-1);
	  write_flush(f_gen);
	  continue;
	}
	break;
      }

      file = &flist->files[i];
      fname = file->name;

      if (local_name)
	fname = local_name;

      if (dry_run) {
	if (!am_server && verbose)
	  printf("%s\n",fname);
	continue;
      }

      if (verbose > 2)
	fprintf(FERROR,"recv_files(%s)\n",fname);

      /* open the file */  
      fd1 = open(fname,O_RDONLY);

      if (fd1 != -1 && fstat(fd1,&st) != 0) {
	fprintf(FERROR,"fstat %s : %s\n",fname,strerror(errno));
	close(fd1);
	return -1;
      }

      if (fd1 != -1 && !S_ISREG(st.st_mode)) {
	fprintf(FERROR,"%s : not a regular file\n",fname);
	close(fd1);
	return -1;
      }

      if (fd1 != -1 && st.st_size > 0) {
	buf = map_file(fd1,st.st_size);
      } else {
	buf = NULL;
      }

      if (verbose > 2)
	fprintf(FERROR,"mapped %s of size %d\n",fname,(int)st.st_size);

      /* open tmp file */
      sprintf(fnametmp,"%s.XXXXXX",fname);
      if (NULL == mktemp(fnametmp)) {
	fprintf(FERROR,"mktemp %s failed\n",fnametmp);
	return -1;
      }
      fd2 = open(fnametmp,O_WRONLY|O_CREAT,file->mode);
      if (fd2 == -1) {
	fprintf(FERROR,"open %s : %s\n",fnametmp,strerror(errno));
	return -1;
      }
      
      cleanup_fname = fnametmp;

      if (!am_server && verbose)
	printf("%s\n",fname);

      /* recv file data */
      receive_data(f_in,buf,fd2,fname);

      if (fd1 != -1) {
	unmap_file(buf);
	close(fd1);
      }
      close(fd2);

      if (verbose > 2)
	fprintf(FERROR,"renaming %s to %s\n",fnametmp,fname);

      if (make_backups) {
	char fnamebak[MAXPATHLEN];
	sprintf(fnamebak,"%s%s",fname,backup_suffix);
	if (rename(fname,fnamebak) != 0 && errno != ENOENT) {
	  fprintf(FERROR,"rename %s %s : %s\n",fname,fnamebak,strerror(errno));
	  exit_cleanup(1);
	}
      }

      /* move tmp file over real file */
      if (rename(fnametmp,fname) != 0) {
	fprintf(FERROR,"rename %s -> %s : %s\n",
		fnametmp,fname,strerror(errno));
      }

      cleanup_fname = NULL;

      set_perms(fname,file,NULL,0);
    }

  if (verbose > 2)
    fprintf(FERROR,"recv_files finished\n");
  
  return 0;
}



off_t send_files(struct file_list *flist,int f_out,int f_in)
{ 
  int fd;
  struct sum_struct *s;
  struct map_struct *buf;
  struct stat st;
  char fname[MAXPATHLEN];  
  off_t total=0;
  int i;
  struct file_struct *file;
  int phase = 0;

  if (verbose > 2)
    fprintf(FERROR,"send_files starting\n");

  setup_nonblocking(f_in,f_out);

  while (1) 
    {
      i = read_int(f_in);
      if (i == -1) {
	if (phase==0 && remote_version >= 13) {
	  phase++;
	  write_int(f_out,-1);
	  write_flush(f_out);
	  continue;
	}
	break;
      }

      file = &flist->files[i];

      fname[0] = 0;
      if (file->dir) {
	strcpy(fname,file->dir);
	strcat(fname,"/");
      }
      strcat(fname,file->name);

      if (verbose > 2) 
	fprintf(FERROR,"send_files(%d,%s)\n",i,fname);

      if (dry_run) {	
	if (!am_server && verbose)
	  printf("%s\n",fname);
	write_int(f_out,i);
	continue;
      }

      s = receive_sums(f_in);
      if (!s) {
	fprintf(FERROR,"receive_sums failed\n");
	return -1;
      }

      fd = open(fname,O_RDONLY);
      if (fd == -1) {
	fprintf(FERROR,"send_files failed to open %s: %s\n",
		fname,strerror(errno));
	continue;
      }
  
      /* map the local file */
      if (fstat(fd,&st) != 0) {
	fprintf(FERROR,"fstat failed : %s\n",strerror(errno));
	return -1;
      }
      
      if (st.st_size > 0) {
	buf = map_file(fd,st.st_size);
      } else {
	buf = NULL;
      }

      if (verbose > 2)
	fprintf(FERROR,"send_files mapped %s of size %d\n",
		fname,(int)st.st_size);

      write_int(f_out,i);

      write_int(f_out,s->count);
      write_int(f_out,s->n);
      write_int(f_out,s->remainder);

      if (verbose > 2)
	fprintf(FERROR,"calling match_sums %s\n",fname);

      if (!am_server && verbose)
	printf("%s\n",fname);
      
      match_sums(f_out,s,buf,st.st_size);
      write_flush(f_out);
      
      unmap_file(buf);
      close(fd);

      free_sums(s);

      if (verbose > 2)
	fprintf(FERROR,"sender finished %s\n",fname);

      total += st.st_size;
    }

  if (verbose > 2)
    fprintf(FERROR,"send files finished\n");

  match_report();

  write_int(f_out,-1);
  write_flush(f_out);

  return total;
}



void generate_files(int f,struct file_list *flist,char *local_name,int f_recv)
{
  int i;

  if (verbose > 2)
    fprintf(FERROR,"generator starting pid=%d count=%d\n",
	    (int)getpid(),flist->count);

  for (i = 0; i < flist->count; i++) {
    struct file_struct *file = &flist->files[i];
    if (!file->name) continue;
    if (S_ISDIR(file->mode)) {
      if (dry_run) continue;
      if (mkdir(file->name,file->mode) != 0 &&
	  errno != EEXIST) {
	fprintf(FERROR,"mkdir %s : %s\n",
		file->name,strerror(errno));
      }
      continue;
    }
    recv_generator(local_name?local_name:file->name,
		   flist,i,f);
  }

  write_int(f,-1);
  write_flush(f);

  if (remote_version >= 13) {
    /* go to the full checksum if anything has failed so far */
    csum_length = SUM_LENGTH;

    for (i=read_int(f_recv); i != -1; i=read_int(f_recv)) {
      struct file_struct *file = &flist->files[i];
      recv_generator(local_name?local_name:file->name,
		     flist,i,f);    
    }

    write_int(f,-1);
    write_flush(f);
  }


  if (verbose > 2)
    fprintf(FERROR,"generator wrote %d\n",write_total());
}


