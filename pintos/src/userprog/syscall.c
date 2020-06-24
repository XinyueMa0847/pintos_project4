#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/exception.h"
#include "userprog/process.h"
#include "devices/shutdown.h"
#include "lib/string.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "filesys/file.h"
#include "filesys/directory.h"
#include "userprog/pagedir.h"
static void syscall_handler (struct intr_frame *);
static struct lock file_lock;

/* Read a byte at user virtual address UADDR, which must be below PHYS_BASE.
  Returns the byte value if successful, -1 if a segfault occured */
static int
get_user (const uint8_t *uaddr)
{
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}

void
file_lock_acquire()
{
  lock_acquire(&file_lock);
}

void
file_lock_release()
{
  lock_release(&file_lock);
}


/*read ARGCth argument from the stack, it could be an int or another
    pointer*/
static int
read_arg(void* esp, int* arg)
{
  if(!is_user_vaddr(esp))
    {
      //printf("invalid pointer passed to system call\n");
      thread_current()->exit_status = -1;
      thread_exit();
    }
  if(pagedir_get_page(thread_current()->pagedir, esp)==NULL)
    {
      thread_current()->exit_status = -1;
      thread_exit();
    }

  /*call get_user 4 times to read four consecutive bytes*/
  int b0, b1,b2,b3;
  b0 = get_user(esp);
  b1 = get_user(esp+1);
  b2 = get_user(esp+2);
  b3 = get_user(esp+3);
   
  /*make sure all four bytes are valid*/
  if(b0==-1||b1==-1||b2==-1||b3==-1)
    { return -1;}
  /*construct arg*/
  else
    {
      *arg= ((uint8_t)b0)|((uint8_t)b1<<8)|
	((uint8_t)b2<<16)|((uint8_t)b3<<24);
    }
  return 0;
}


static int
sys_exit(void* esp)
{
  int exit_status;
  if(read_arg((esp+sizeof(int)),&exit_status)==-1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  else{thread_current()->exit_status = exit_status;}
  thread_exit();
  return exit_status;
}

static int
sys_exec(void* esp)
{
  int filename,check;
  if(read_arg((esp+sizeof(int)),&filename)==-1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }  
  if(read_arg((void*)filename,&check)==-1||(void*)filename==NULL||strlen((char*)filename)<1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  return process_execute((const char*)filename); 
}

static int
sys_wait(void* esp)
{
  int child_tid;
  /*idle thread and main thread cannot be waited*/
  if(read_arg((esp+sizeof(int)),&child_tid)==-1||child_tid<2)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  return process_wait((tid_t)child_tid);
}

static bool
sys_create(void* esp)
{
  int file, size,check;
  if(read_arg((esp+sizeof(int)),&file)==-1 ||
     read_arg((esp+sizeof(int)*2),&size)==-1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  if(read_arg((void*)file,&check)==-1||size<0||(void*)file==NULL||strlen((char*)file)<1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  //lock_acquire(&file_lock);
  bool ret = filesys_create( (char*)file, size);
  //lock_release(&file_lock);
  return ret;
}

static int
sys_open(void* esp)
{
  int file,check;

  if(read_arg((esp+sizeof(int)),&file)==-1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  if(read_arg((void*)file,&check)==-1||(void*)file==NULL)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  if(strlen((char*)file)<1)
    {
      return -1;
    }
  // lock_acquire(&file_lock);
  struct file* ret = filesys_open( (char*)file);
  if(ret) {
    thread_current()->fdt[thread_current()->next_fd++]=ret;
    if( strcmp((char*)file,(char*)thread_current()->name) == 0)
      {
	file_deny_write(ret);
      }
    // lock_release(&file_lock);
    return thread_current()->next_fd-1;
  }
  else{
    // lock_release(&file_lock);
    return -1;
  }
}

static bool
sys_remove(void* esp)
{
  int file,check;
  bool ret;
  if(read_arg((esp+sizeof(int)),&file)==-1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  if(read_arg((void*)file,&check)==-1||(void*)file==NULL)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  if(strlen((char*)file)<1)
    {
      return -1;
    }
  //lock_acquire(&file_lock);
  ret = filesys_remove((char*)file);
  //lock_release(&file_lock);
  return ret;
}

static void
sys_close(void* esp)
{
  int fd;

  if(read_arg((esp+sizeof(int)),&fd)==-1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  if(fd<thread_current()->next_fd)
    {
      struct file* file = thread_current()->fdt[fd];
      if(file == NULL){  
	thread_current()->exit_status=-1;
	thread_exit();
      }
      else{
	//lock_acquire(&file_lock);
	file_close(file);
	//lock_release(&file_lock);
      }
      thread_current()->fdt[fd] = NULL;
    }
  else
    {
      thread_current()->exit_status=-1;
      thread_exit();      
    }  
}

static int
sys_filesize(void* esp)
{
  int fd;
  int size = -1;
  if(read_arg((esp+sizeof(int)),&fd)==-1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  if(fd<thread_current()->next_fd)
    {
      struct file* file = thread_current()->fdt[fd];
      if(file == NULL){  
	thread_current()->exit_status=-1;
	thread_exit();
      }
      else{
	//lock_acquire(&file_lock);
	size = file_length(file);
	//lock_release(&file_lock);
      }
    }
  else
    {
      thread_current()->exit_status=-1;
      thread_exit();      
    }  
  return size;
}

static int
sys_tell(void* esp)
{
  int fd;
  int pos =0;
  if(read_arg((esp+sizeof(int)),&fd)==-1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  if(fd<thread_current()->next_fd)
    {
      struct file* file = thread_current()->fdt[fd];
      if(file == NULL){  
	thread_current()->exit_status=-1;
	thread_exit();
      }
      else{
	//lock_acquire(&file_lock);
	pos = file_tell(file);
	//lock_release(&file_lock);
      }
    }
  else
    {
      thread_current()->exit_status=-1;
      thread_exit();      
    }  
  return pos;
}



static int
sys_write(void* esp)
{

  /* read arguments*/
  int fd,size,buf_ptr, check;
  int bytes = 0;
  if(read_arg((esp+sizeof(int)),&fd)==-1 ||
     read_arg((esp+sizeof(int)*2),&buf_ptr)==-1 ||
     read_arg((esp+sizeof(int)*3),&size)==-1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  if(read_arg((void*)buf_ptr,&check)==-1||size<0||fd<0)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }

  if(fd==1){
    //lock_acquire(&file_lock);
    putbuf ((const void *)buf_ptr,size);
    //lock_release(&file_lock);
    return size;
  }
  else if(fd==0)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  else
    {
     if(fd >= thread_current()->next_fd)
	{
	  thread_current()->exit_status=-1;
	  thread_exit();
	}

      struct file* file = thread_current()->fdt[fd];
      if(file==NULL){
	thread_current()->exit_status=-1;
	thread_exit();
      }      
      //lock_acquire(&file_lock);
      bytes = file_write(file,(void*)buf_ptr,size);
      //lock_release(&file_lock);
    }
  return bytes;
}

static int
sys_read(void* esp)
{

  /* read arguments*/
  int fd,size,buf_ptr,check;
  int bytes = 0;
  if(read_arg((esp+sizeof(int)),&fd)==-1 ||
     read_arg((esp+sizeof(int)*2),&buf_ptr)==-1 ||
     read_arg((esp+sizeof(int)*3),&size)==-1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  if(read_arg((void*)buf_ptr,&check)==-1||size<0||fd<0)
    {      
      thread_current()->exit_status=-1;
      thread_exit();
    }

  if(fd==0){
    lock_acquire(&file_lock);
    for(bytes=0;bytes<size;bytes++)
      {
	char* charbuf = (char*)buf_ptr;
	charbuf[bytes]=input_getc();
      }
    lock_release(&file_lock);
    return bytes;
  }
  else if(fd==1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  else
    {
      if(fd >= thread_current()->next_fd)
	{
	  thread_current()->exit_status=-1;
	  thread_exit();
	}
      struct file* file = thread_current()->fdt[fd];
      if(file==NULL){
	thread_current()->exit_status=-1;
	thread_exit();
      }      
      // lock_acquire(&file_lock);
      bytes = file_read(file,(void*)buf_ptr,size);
      //lock_release(&file_lock);
    }

  return bytes;
}

static void
sys_seek(void* esp)
{

  /* read arguments*/
  int fd,new_pos;
  if(read_arg((esp+sizeof(int)),&fd)==-1 ||
     read_arg((esp+sizeof(int)*2),&new_pos)==-1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  if(new_pos<0||fd<0)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  else
    {
      struct file* file = thread_current()->fdt[fd];
      if(file==NULL){
	thread_current()->exit_status=-1;
	thread_exit();
      }     
      //lock_acquire(&file_lock);
      file_seek(file,new_pos);
      // lock_release(&file_lock);
    }
}

static bool sys_mkdir(void* esp){
  int dir,check;
  if(read_arg((esp+sizeof(int)),&dir)==-1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  if(read_arg((void*)dir,&check)==-1||(void*)dir==NULL)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  return dir_mkdir((char*)dir);
}

static bool sys_chdir(void* esp){
  int dir,check;
  if(read_arg((esp+sizeof(int)),&dir)==-1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  if(read_arg((void*)dir,&check)==-1||(void*)dir==NULL)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  return dir_chdir((const char*)dir);
}

static bool sys_readdir(void* esp){
  /* read arguments*/
  int fd,name, check;
  if(read_arg((esp+sizeof(int)),&fd)==-1 ||
     read_arg((esp+sizeof(int)*2),&name)==-1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  if(read_arg((void*)name,&check)==-1||(char*)name==NULL)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    }
  /* validation */
  if(fd < 3)
    return false;
  struct file* file = thread_current()->fdt[fd];
  if(file==NULL)
    return false;
  struct inode* inode = file_get_inode(file);
  if(inode==NULL)
    return false;
  struct dir* dir = dir_open(inode);
  if(dir==NULL) 
    return false; 
  return dir_readdir(dir,(char*)name);
  
}

static bool sys_isdir(void* esp){
  int fd;
  if(read_arg((esp+sizeof(int)),&fd)==-1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    };
  if(fd < 3)
    return false;
  struct file* file = thread_current()->fdt[fd];
  if(file==NULL)
    return false;
  struct inode* inode = file_get_inode(file);
  if(inode==NULL)
    return false;
  return is_dir(inode);
  
}

static int sys_inumber(void* esp){
  int fd;
  if(read_arg((esp+sizeof(int)),&fd)==-1)
    {
      thread_current()->exit_status=-1;
      thread_exit();
    };
  if(fd < 3)
    return false;
  struct file* file = thread_current()->fdt[fd];
  if(file==NULL)
    return false;
  struct inode* inode = file_get_inode(file);
  if (inode==NULL)
    return false;
  return (int)inode_get_inumber(inode);
}
void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&file_lock);
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
   int syscall_num;
  /* validate pointer*/
  if(read_arg(f->esp,&syscall_num)==-1){thread_exit();}
  /*validate syscall number*/
  else
    {
      if(syscall_num<1||syscall_num>19){f->eax=-1;}
      else
  	{
  	  switch(syscall_num)
  	    {
  	      /* Halt the operating system. */
  	    case SYS_HALT:
  	      shutdown_power_off();
  	      break;
  	    case SYS_EXIT: /* Terminate this process. */
  	      f->eax = sys_exit(f->esp);
  	      break;
  	    case SYS_EXEC:                   /* Start another process. */
	      f->eax = sys_exec(f->esp);
  	      break;
  	    case SYS_WAIT:                   /* Wait for a child process to die. */
	      f->eax = sys_wait(f->esp);
  	      break;
  	    case SYS_CREATE:                 /* Create a file. */
  	      f->eax = sys_create(f->esp);
  	      break;
  	    case SYS_REMOVE:                 /* Delete a file. */
  	      f->eax = sys_remove(f->esp);
  	      break;
  	    case SYS_OPEN:                   /* Open a file. */
  	      f->eax = sys_open(f->esp);
	      break;
  	    case SYS_FILESIZE:               /* Obtain a file's size. */
  	       f->eax = sys_filesize(f->esp);
  	      break;
  	    case SYS_READ:                   /* Read from a file. */
	      f->eax = sys_read(f->esp);
  	      break;
  	    case SYS_WRITE:                  /* Write to a file. */
  	      f->eax = sys_write(f->esp);
  	      break;
  	    case SYS_SEEK:                   /* Change position in a file. */
  	      sys_seek(f->esp);
  	      break;
  	    case SYS_TELL:                   /* Report current position in a file. */
  	      f->eax = sys_tell(f->esp);
	      break;
  	    case SYS_CLOSE:                  /* Close a file. */
  	      sys_close(f->esp);
  	      break;
	    case SYS_MKDIR:
	      f->eax = sys_mkdir(f->esp);
	      break;
	    case SYS_CHDIR:
	      f->eax = sys_chdir(f->esp);	      
	      break;
	    case SYS_READDIR:
	       f->eax = sys_readdir(f->esp);
	      break;
	    case SYS_ISDIR:
	      f->eax = sys_isdir(f->esp);
	      break;
	    case SYS_INUMBER:
	      f->eax = sys_inumber(f->esp);
	      break;
  	    }
  	}
    }

}
