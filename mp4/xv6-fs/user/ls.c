#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/fs.h"

char*
fmtname(char *path, char *symlink)
{
  static char buf[MAXPATH];
  char *p;

  // Find first character after last slash.
  for(p=path+strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;

  // append symbolic link
  strcpy(buf, p);
  if(strlen(symlink)) {
    strcat(buf, " -> ");
    strcat(buf, symlink);
  }

  return buf;
}

void
ls(char *path)
{
  char buf[512], *p, t;
  char symlink[MAXPATH];
  int fd;
  struct dirent de;
  struct stat st;

  if((fd = open(path, O_RDONLY | O_NOFOLLOW)) < 0){
    fprintf(2, "ls: cannot open %s\n", path);
    return;
  }

  if(fstat(fd, &st) < 0){
    fprintf(2, "ls: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch(st.type){
  case T_FILE:
    printf("%c %d\t%d\t%d\t%s\n", '-', st.type, st.ino, st.size, fmtname(path, ""));
    break;

  case T_DEVICE:
    printf("%c %d\t%d\t%d\t%s\n", 'c', st.type, st.ino, st.size, fmtname(path, ""));
    break;

  case T_SYMLINK:
    if(read(fd, symlink, MAXPATH) != MAXPATH)
      symlink[0] = '\0';

    printf("%c %d\t%d\t%d\t%s\n", 'l', st.type, st.ino, st.size, fmtname(path, symlink));
    symlink[0] = '\0';
    break;

  case T_DIR:
    if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
      printf("ls: path too long\n");
      break;
    }
    strcpy(buf, path);
    p = buf+strlen(buf);
    *p++ = '/';
    while(read(fd, &de, sizeof(de)) == sizeof(de)){
      if(de.inum == 0)
        continue;
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;
      if(stat(buf, &st) < 0){
        printf("ls: cannot stat %s\n", buf);
        continue;
      }
      switch(st.type){
      case T_DIR: t = 'd'; break;
      case T_FILE: t = '-'; break;
      case T_DEVICE: t = 'c'; break;
      case T_SYMLINK:
      {
        int fd;

        t = 'l';

        fd = open(buf, O_RDONLY | O_NOFOLLOW);
        if(fd < 0)
          break;
        if(read(fd, symlink, MAXPATH) != MAXPATH)
          symlink[0] = '\0';
        close(fd);

        break;
      }
      default: t = ' '; break;
      }
      printf("%c %d\t%d\t%d\t%s\n", t, st.type, st.ino, st.size, fmtname(buf, symlink));
      symlink[0] = '\0';
    }
    break;
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  int i;

  if(argc < 2){
    ls(".");
    exit(0);
  }
  for(i=1; i<argc; i++)
    ls(argv[i]);
  exit(0);
}
