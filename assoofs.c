#include <linux/module.h>       /* Needed by all modules */
#include <linux/kernel.h>       /* Needed for KERN_INFO  */
#include <linux/init.h>         /* Needed for the macros */
#include <linux/fs.h>           /* libfs stuff           */
#include <linux/buffer_head.h>  /* buffer_head           */
#include <linux/slab.h>         /* kmem_cache            */
#include "assoofs.h"

/*
 *  Operaciones sobre ficheros
 */
struct assoofs_inode_info *assoofs_get_inode_info(struct super_block *sb, uint64_t inode_no);
static struct inode *assoofs_get_inode(struct super_block *sb, int ino);
int assoofs_sb_get_a_freeblock(struct super_block *sb, uint64_t *block);
void assoofs_save_sb_info(struct super_block *vsb);
void assoofs_add_inode_info(struct super_block *sb, struct assoofs_inode_info *inode);
int assoofs_save_inode_info(struct super_block *sb, struct assoofs_inode_info *inode_info);
struct assoofs_inode_info *assoofs_search_inode_info(struct super_block *sb, struct assoofs_inode_info *start, struct assoofs_inode_info *search);
ssize_t assoofs_read(struct file * filp, char __user * buf, size_t len, loff_t * ppos);
ssize_t assoofs_write(struct file * filp, const char __user * buf, size_t len, loff_t * ppos);
const struct file_operations assoofs_file_operations = {
    .read = assoofs_read,
    .write = assoofs_write,
};

ssize_t assoofs_read(struct file * filp, char __user * buf, size_t len, loff_t * ppos) {
    printk(KERN_INFO "Read request\n");

    struct assoofs_inode_info *inode_info = filp->f_path.dentry->d_inode->i_private;
    if (*ppos >= inode_info->file_size) return 0;
    struct buffer_head *bh;
	char *buffer;

    bh = sb_bread(filp->f_path.dentry->d_inode->i_sb, inode_info->data_block_number);
	buffer = (char *)bh->b_data;

	int nbytes;
	nbytes = min((size_t) inode_info->file_size, len);
	copy_to_user(buf, buffer, nbytes);

	*ppos += nbytes;
	return nbytes;
}

ssize_t assoofs_write(struct file * filp, const char __user * buf, size_t len, loff_t * ppos) {
    printk(KERN_INFO "Write request\n");

    struct assoofs_inode_info *inode_info = filp->f_path.dentry->d_inode->i_private;
    struct buffer_head *bh;
    struct super_block *sb = filp->f_path.dentry->d_inode->i_sb;
	char *buffer;

	bh = sb_bread(sb, inode_info->data_block_number);
    buffer = (char *)bh->b_data;
	buffer += *ppos;
	copy_from_user(buffer, buf, len);

	*ppos += len;
	inode_info->file_size = *ppos;


	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	assoofs_save_inode_info(sb, inode_info);



    return len;
}

/*
 *  Operaciones sobre directorios
 */
static int assoofs_iterate(struct file *filp, struct dir_context *ctx);
const struct file_operations assoofs_dir_operations = {
    .owner = THIS_MODULE,
    .iterate = assoofs_iterate,
};

static int assoofs_iterate(struct file *filp, struct dir_context *ctx) {
    printk(KERN_INFO "Iterate request\n");
    struct inode *inode;
    struct assoofs_inode_info *inode_info;
    struct assoofs_dir_record_entry *record;
    struct super_block *sb;

	
	inode = filp->f_path.dentry->d_inode;
	sb = inode->i_sb;
	inode_info = inode->i_private;

	if (ctx->pos) return 0;
	if ((!S_ISDIR(inode_info->mode))) return -1;

	struct buffer_head *bh;
	bh = sb_bread(sb, inode_info->data_block_number);
	record = (struct assoofs_dir_record_entry *)bh->b_data;
	int i;
	for (i = 0; i < inode_info->dir_children_count; i++) {
		dir_emit(ctx, record->filename, ASSOOFS_FILENAME_MAXLEN, record->inode_no, DT_UNKNOWN);
		ctx->pos += sizeof(struct assoofs_dir_record_entry);
		record++;
	}
	brelse(bh);

    return 0;
}

/*
 *  Operaciones sobre inodos
 */
static int assoofs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl);
struct dentry *assoofs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags);
static int assoofs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode);
static struct inode_operations assoofs_inode_ops = {
    .create = assoofs_create,
    .lookup = assoofs_lookup,
    .mkdir = assoofs_mkdir,
};

struct dentry *assoofs_lookup(struct inode *parent_inode, struct dentry *child_dentry, unsigned int flags) {
	printk(KERN_INFO "Lookup request\n");
	struct assoofs_inode_info *parent_info = parent_inode->i_private;
	struct super_block *sb = parent_inode->i_sb;
	struct buffer_head *bh;
	bh = sb_bread(sb, parent_info->data_block_number);

	struct assoofs_dir_record_entry *record;
	record = (struct assoofs_dir_record_entry *)bh->b_data;
	int i;
	for (i=0; i < parent_info->dir_children_count; i++) {
		if (!strcmp(record->filename, child_dentry->d_name.name)) {
			struct inode *inode = assoofs_get_inode(sb, record->inode_no);
			inode_init_owner(inode, parent_inode, ((struct assoofs_inode_info *)inode->i_private)->mode);
			d_add(child_dentry, inode);
			return NULL;
		}
		record++;

	}

    
    return NULL;
}

static struct inode *assoofs_get_inode(struct super_block *sb, int ino){

	struct inode *inode;
	inode = new_inode(sb);
	struct assoofs_inode_info *inode_info = assoofs_get_inode_info(sb, ino);

	inode->i_ino = ino;
	inode->i_sb = sb; 
    inode->i_op = &assoofs_inode_ops; 
    inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
    inode->i_private = inode_info;

	if (S_ISDIR(inode_info->mode))

		inode->i_fop = &assoofs_dir_operations;

	else if (S_ISREG(inode_info->mode))

		inode->i_fop = &assoofs_file_operations;

	else

	printk(KERN_ERR "Unknown inode type. Neither a directory nor a file.");


	return inode;
}

static int assoofs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
	//1
	struct inode *inode;
	struct super_block *sb;
	uint64_t count;
	sb = dir->i_sb;

	count = ((struct assoofs_super_block_info *)sb->s_fs_info)->inodes_count; 
	if(count >= ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED-2){
		printk(KERN_ERR "Numero maximo alcanzado\n");
		return -1;
	}
	inode = new_inode(sb);
	inode->i_sb = sb;
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
	inode->i_op = &assoofs_inode_ops;
	inode->i_ino = count + 1; 

	struct assoofs_inode_info *inode_info;
	inode_info = kmalloc(sizeof(struct assoofs_inode_info), GFP_KERNEL);
	inode_info->inode_no = inode->i_ino;
	inode_info->mode = mode;
	inode_info->file_size = 0;
	inode->i_private = inode_info;
	inode->i_fop=&assoofs_file_operations;

	inode_init_owner(inode, dir, mode);	
	d_add(dentry, inode);

	assoofs_sb_get_a_freeblock(sb, &inode_info->data_block_number);
	assoofs_add_inode_info(sb, inode_info);

	//2

	struct assoofs_inode_info *parent_inode_info;
	struct assoofs_dir_record_entry *dir_contents;
	struct buffer_head *bh;

	parent_inode_info = dir->i_private;
	bh = sb_bread(sb, parent_inode_info->data_block_number);

	dir_contents = (struct assoofs_dir_record_entry *)bh->b_data;
	dir_contents += parent_inode_info->dir_children_count;
	dir_contents->inode_no = inode_info->inode_no;

	strcpy(dir_contents->filename, dentry->d_name.name);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	//3

	parent_inode_info->dir_children_count++;
	assoofs_save_inode_info(sb, parent_inode_info);

    printk(KERN_INFO "New file request\n");
    return 0;
}

int assoofs_save_inode_info(struct super_block *sb, struct assoofs_inode_info *inode_info){

	struct buffer_head *bh;
    bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
    struct assoofs_inode_info *inode_pos = assoofs_search_inode_info(sb, (struct assoofs_inode_info *)bh->b_data, inode_info);

    memcpy(inode_pos, inode_info, sizeof(*inode_pos));
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);
    return 0;
}

struct assoofs_inode_info *assoofs_search_inode_info(struct super_block *sb, struct assoofs_inode_info *start, struct assoofs_inode_info *search){

	uint64_t count = 0;
	while (start->inode_no != search->inode_no && count < ((struct assoofs_super_block_info *)sb->s_fs_info)->inodes_count) {
		count++;
		start++;
	}

	if (start->inode_no == search->inode_no){
		return start;
	} else {
		return NULL;
	}

}

int assoofs_sb_get_a_freeblock(struct super_block *sb, uint64_t *block){
	struct assoofs_super_block_info *assoofs_sb = sb->s_fs_info;

	int i;
	for (i = 2; i < ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED; i++){
		if (assoofs_sb->free_blocks & (1 << i)){
			break;
		}
	}
	if(i >= ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED-2){
		printk(KERN_ERR "Numero maximo alcanzado\n");
		return -1;
	}

	*block = i; 

	assoofs_sb->free_blocks &= ~(1 << i);
	assoofs_save_sb_info(sb);
	return 0;
}

void assoofs_save_sb_info(struct super_block *vsb){
	struct buffer_head *bh;
	struct assoofs_super_block *sb = vsb->s_fs_info;
	bh = sb_bread(vsb, ASSOOFS_SUPERBLOCK_BLOCK_NUMBER);
	bh->b_data = (char *)sb;

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
}

void assoofs_add_inode_info(struct super_block *sb, struct assoofs_inode_info *inode){
	struct buffer_head *bh;
	struct assoofs_inode_info *inode_info;
	struct assoofs_super_block_info *assoofs_sb = sb->s_fs_info;
	
	bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);

	inode_info = (struct assoofs_inode_info *)bh->b_data;
	inode_info += assoofs_sb->inodes_count;
	memcpy(inode_info, inode, sizeof(struct assoofs_inode_info));

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);

	assoofs_sb->inodes_count++;
	assoofs_save_sb_info(sb);

}

static int assoofs_mkdir(struct inode *dir , struct dentry *dentry, umode_t mode) {
    //1
	struct inode *inode;
	struct super_block *sb;
	uint64_t count;
	sb = dir->i_sb;

	count = ((struct assoofs_super_block_info *)sb->s_fs_info)->inodes_count; 
	if(count >= ASSOOFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED-2){
		printk(KERN_ERR "Numero maximo alcanzado\n");
		return -1;
	}
	inode = new_inode(sb);
	inode->i_sb = sb;
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
	inode->i_op = &assoofs_inode_ops;
	inode->i_ino = count + 1; 

	struct assoofs_inode_info *inode_info;
	inode_info = kmalloc(sizeof(struct assoofs_inode_info), GFP_KERNEL);
	inode_info->inode_no = inode->i_ino;
	inode_info->dir_children_count = 0;
	inode_info->mode = S_IFDIR | mode;
	inode_info->file_size = 0;
	inode->i_private = inode_info;
	inode->i_fop=&assoofs_dir_operations;

	inode_init_owner(inode, dir, S_IFDIR | mode);	
	d_add(dentry, inode);

	assoofs_sb_get_a_freeblock(sb, &inode_info->data_block_number);
	assoofs_add_inode_info(sb, inode_info);

	//2

	struct assoofs_inode_info *parent_inode_info;
	struct assoofs_dir_record_entry *dir_contents;
	struct buffer_head *bh;

	parent_inode_info = dir->i_private;
	bh = sb_bread(sb, parent_inode_info->data_block_number);

	dir_contents = (struct assoofs_dir_record_entry *)bh->b_data;
	dir_contents += parent_inode_info->dir_children_count;
	dir_contents->inode_no = inode_info->inode_no;

	strcpy(dir_contents->filename, dentry->d_name.name);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	//3

	parent_inode_info->dir_children_count++;
	assoofs_save_inode_info(sb, parent_inode_info);

    printk(KERN_INFO "New file request\n");
    return 0;
}

/*
 *  Operaciones sobre el superbloque
 */
static const struct super_operations assoofs_sops = {
    .drop_inode = generic_delete_inode,
};

/*
 *  Inicialización del superbloque
 */
int assoofs_fill_super(struct super_block *sb, void *data, int silent) {   
    printk(KERN_INFO "assoofs_fill_super request\n");
    // 1.- Leer la información persistente del superbloque del dispositivo de bloques
    struct buffer_head *bh;
    struct assoofs_super_block_info *assoofs_sb;
    bh = sb_bread(sb, ASSOOFS_SUPERBLOCK_BLOCK_NUMBER); // sb lo recibe assoofs_fill_super como argumento
    assoofs_sb = (struct assoofs_super_block_info *)bh->b_data;
    brelse(bh);
    // 2.- Comprobar los parámetros del superbloque
    if(assoofs_sb->magic == ASSOOFS_MAGIC){
    	printk(KERN_INFO "Numero magico correcto\n");
    }
    if(assoofs_sb->block_size == ASSOOFS_DEFAULT_BLOCK_SIZE){
    	printk(KERN_INFO "Tamanio de bloque correcto\n");
    }

    // 3.- Escribir la información persistente leída del dispositivo de bloques en el superbloque sb, incluído el campo s_op con las operaciones que soporta.
    sb->s_magic = ASSOOFS_MAGIC;
    sb->s_maxbytes = ASSOOFS_DEFAULT_BLOCK_SIZE;
    sb->s_op = &assoofs_sops;
    sb->s_fs_info = assoofs_sb;
    // 4.- Crear el inodo raíz y asignarle operaciones sobre inodos (i_op) y sobre directorios (i_fop)
    struct inode *root_inode;
    root_inode = new_inode(sb);
    inode_init_owner(root_inode, NULL, S_IFDIR); // S_IFDIR para directorios, S_IFREG para ficheros.

    root_inode->i_ino = ASSOOFS_ROOTDIR_INODE_NUMBER; // numero de inodo
    root_inode->i_sb = sb; // puntero al superbloque
    root_inode->i_op = &assoofs_inode_ops; // direccion de una variable de tipo struct inode_operations previamente declarada
    root_inode->i_fop = &assoofs_dir_operations; 
    root_inode->i_atime = root_inode->i_mtime = root_inode->i_ctime = current_time(root_inode); // fechas.
    root_inode->i_private = assoofs_get_inode_info(sb, ASSOOFS_ROOTDIR_INODE_NUMBER);

    
    sb->s_root = d_make_root(root_inode);
    

    return 0;
}

struct assoofs_inode_info *assoofs_get_inode_info(struct super_block *sb, uint64_t inode_no){

	struct assoofs_inode_info *inode_info = NULL;
	struct buffer_head *bh;

	bh = sb_bread(sb, ASSOOFS_INODESTORE_BLOCK_NUMBER);
	inode_info = (struct assoofs_inode_info *)bh->b_data;

	struct assoofs_super_block_info *afs_sb = sb->s_fs_info;
	struct assoofs_inode_info *buffer = NULL;
	int i;
	for (i = 0; i < afs_sb->inodes_count; i++) {

		if (inode_info->inode_no == inode_no) {

			buffer = kmalloc(sizeof(struct assoofs_inode_info), GFP_KERNEL);
			memcpy(buffer, inode_info, sizeof(*buffer));
			break;

		}

		inode_info++;

	}

	brelse(bh);
	return buffer;
}

/*
 *  Montaje de dispositivos assoofs
 */
static struct dentry *assoofs_mount(struct file_system_type *fs_type, int flags, const char *dev_name, void *data) {
    printk(KERN_INFO "assoofs_mount request\n");
    struct dentry *ret = mount_bdev(fs_type, flags, dev_name, data, assoofs_fill_super);
    // Control de errores a partir del valor de ret. En este caso se puede utilizar la macro IS_ERR: if (IS_ERR(ret)) ...
    return ret;
}

/*
 *  assoofs file system type
 */
static struct file_system_type assoofs_type = {
    .owner   = THIS_MODULE,
    .name    = "assoofs",
    .mount   = assoofs_mount,
    .kill_sb = kill_litter_super,
};

static int __init assoofs_init(void) {
    printk(KERN_INFO "assoofs_init request\n");
    int ret = register_filesystem(&assoofs_type);
    // Control de errores a partir del valor de ret
}

static void __exit assoofs_exit(void) {
    printk(KERN_INFO "assoofs_exit request\n");
    int ret = unregister_filesystem(&assoofs_type);
    // Control de errores a partir del valor de ret
}

//extern int register_filesystem(struct file_system_type *);
//extern int unregister_filesystem(struct file_system_type *);
//extern struct dentry *mount_bdev(struct file_system_type *fs_type,
//	int flags, const char *dev_name, void *data,
//	int (*fill_super)(struct super_block *, void *, int));
//static inline struct buffer_head *sb_bread(struct super_block *sb, sector_t block){
//	return __bread(sb->s_bdev, block, sb->s_blocksize);
//}

module_init(assoofs_init);
module_exit(assoofs_exit);
