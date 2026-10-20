/*
 * extents FILE: the file's extents on its volume as "first_4k_block count"
 * lines (F_LOG2PHYS_EXT), for writing a file's own blocks in place on the
 * phone (tools/sweetgpu-ca/push.sh, tools/sweetgl/push.sh). Run on the
 * shadow-mounted pull of sda35: offsets are relative to that volume.
 */
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	int fd = argc == 2 ? open(argv[1], O_RDONLY) : -1;
	struct stat st;
	if (fd < 0 || fstat(fd, &st) != 0) {
		fprintf(stderr, "usage: extents FILE\n");
		return 1;
	}
	for (off_t off = 0; off < st.st_size;) {
		struct log2phys l = {.l2p_contigbytes = st.st_size - off, .l2p_devoffset = off};
		if (fcntl(fd, F_LOG2PHYS_EXT, &l) != 0 || l.l2p_contigbytes <= 0 ||
		    (l.l2p_devoffset | l.l2p_contigbytes) & 4095) {
			fprintf(stderr, "F_LOG2PHYS_EXT failed at %lld\n", (long long)off);
			return 1;
		}
		printf("%lld %lld\n", (long long)(l.l2p_devoffset / 4096), (long long)(l.l2p_contigbytes / 4096));
		off += l.l2p_contigbytes;
	}
	return 0;
}
