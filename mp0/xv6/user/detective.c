#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

//#define DEBUG


int walk(char *commission, char *path)
{
	char buf[512], *p;
	int fd, count = 0;
	struct dirent de;
	struct stat st;

#ifdef DEBUG
	printf("%s: commission '%s', path '%s'\n", __func__, commission, path);
#endif

	if ((fd = open(path, 0)) < 0) {
		fprintf(2, "detective: cannot open %s\n", path);
		return count;
	}

	if (fstat(fd, &st) < 0) {
		fprintf(2, "detective: cannot stat %s\n", path);
		goto _exit;
	}

	if (st.type != T_DIR) {
		/* should not happen */
		goto _exit;
	}

	if (strlen(path) + 1 + DIRSIZ + 1 > sizeof(buf)) {
		fprintf(2, "detective: path too long\n");
		goto _exit;
	}

	strcpy(buf, path);
	p = buf + strlen(buf);
	*p++ = '/';

	while (read(fd, &de, sizeof(de)) == sizeof(de)) {
		if (de.inum == 0)
			continue;

#ifdef DEBUG
		printf("%s: de.name '%s'\n", __func__, de.name);
#endif

		if (!strcmp(de.name, ".") || !strcmp(de.name, ".."))
			continue;

		memmove(p, de.name, DIRSIZ);
		p[DIRSIZ] = 0;
		if (stat(buf, &st) < 0) {
			fprintf(2, "detective: cannot stat %s\n", buf);
			/* should not happen, but we continue anyway */
			continue;
		}

		/* only check file and directory names */
		if ((st.type != T_DIR) && (st.type != T_FILE))
			continue;

		if (!strcmp(commission, de.name)) {
			fprintf(1, "%d as Watson: %s\n", getpid(), buf);
			count += 1;
		}

		if (st.type == T_DIR)
			count += walk(commission, buf);
	}

_exit:
	close(fd);
	return count;
}

int main(int argc, char *argv[])
{
	char *commission;
	int pid, p[2], count;
	char result;

	if (argc != 2) {
		fprintf(2, "usage: detective [commission]\n");
		exit(0);
	}

	commission = argv[1];

	pipe(p); /* 0 is read side, 1 is write side */

	pid = fork();
	if (pid > 0) {
		/* parent */

		/* wait child to exit */
		wait((int *) 0);

		if (read(p[0], &result, sizeof(char)) != 1)
			fprintf(2, "detective: read error\n");
		else if ((result == 'y') || (result == 'n'))
			fprintf(1, "%d as Holmes: This is the %s\n", getpid(),
				result == 'y' ? "evidence" : "alibi");
		else
			fprintf(2, "detective: invalid result\n");
	} else if (pid == 0) {
		/* child */

		/* search from current directory */
		count = walk(commission, ".");
#ifdef DEBUG
		printf("%s: count %d\n", __func__, count);
#endif

		write(p[1], (count == 0) ? "n" : "y", sizeof(char)); /* only 1 char */

	} else
		fprintf(2, "detective: fork error\n");

	/* close the pipe */
	close(p[0]);
	close(p[1]);

	exit(0);
}
