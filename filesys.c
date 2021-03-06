#include "filesys.h"
#include <string.h>
#include "free-map.h"
#include "memcached_client.h"
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>

// TODO: retrieve filesys files from memcached.
void filesys_init(){
    init_connection();
    init_inode();
    if(open_freemap_inode() < 0){
        flush_all();
        create_freemap_inode();
        init_free_map();
        dir_create_root();
        cur_dir = dir_open_root();
        return;
    }

    cur_dir = dir_open_root();
    if(cur_dir == NULL){
        flush_all();
        create_freemap_inode();
        init_free_map();
        dir_create_root();
        cur_dir = dir_open_root();
        return;
    }
    init_free_map();

}

void filesys_finish(){
    dir_close(cur_dir);
    free_map_finish();
    close_connection();
}

// TODO: need to close directories correctly
dir_t* follow_path(const char* path, char** recv_file_name){
    if(path == NULL || strlen(path) < 1 )
        return NULL;
    char path_temp[strlen(path) + 1];
    memcpy(path_temp, path, strlen(path) + 1);
    
    char* delim = "/";
    char* token = strtok(path_temp, delim);
    char* temp_token;

    dir_t* c_dir;
    dir_t* temp;

    if(path[0] == '/'){
        c_dir = dir_open_root();
        if(token == NULL){
            return c_dir;
        }
    }else{
        c_dir = dir_reopen(cur_dir);
    }

    int status = 0;
    int mode = 0;
    while(token != NULL){
        temp_token = strtok(NULL, delim);
        
        // if token is last part
        if(temp_token == NULL){
            *recv_file_name = (char*) (path + (token - path_temp));
            return c_dir;
        }

        mode = dir_get_entry_mode(c_dir, token);
        if(!S_ISDIR(mode)){
        // printf("%s**\n", token);
            dir_close(c_dir);
            return NULL;
        }
        temp = c_dir;
        c_dir = dir_open(c_dir, token);
        dir_close(temp);
        if(c_dir == NULL){
            return NULL;
        }

        token = temp_token;

    }
    // shouldn't come here
    assert(0);
    return NULL;
}

file_info_t* open_file (const char* path){
    char* file_name;
    dir_t* dir = follow_path(path, &file_name);
    if(dir == NULL)
        return NULL;
    inumber_t inumber;
    if(dir_entry_exists(dir, file_name) == 0)
        inumber = dir_get_entry_inumber(dir, file_name);
    else
        return NULL;
    
    inode_t* inode = inode_open(inumber);
    if(inode == NULL){
        dir_close(dir);
        return NULL;
    }

    file_info_t* fi = malloc(sizeof(file_info_t));
    fi->inode = inode;
    dir_close(dir);
    return fi;
}

void close_file (file_info_t* fi){
    if(fi != NULL){
        inode_close(fi->inode);
        free(fi);
    }
}

int write_file_at (file_info_t* fi, void* buf, size_t off, size_t len){
    int status = inode_write(fi->inode, buf, off, len);
    // if (status <){
    //     return len;
    // }
    return status;
}
int read_file_at (file_info_t* fi, void* buf, size_t off, size_t len){
    return inode_read(fi->inode, buf, off, len);
}

int getattr_inode (inode_t* inode){
    return inode->d_inode.mode;
}

int getattr_path (const char* path){
    char* file_name;
    dir_t* dir = follow_path(path, &file_name);
    int mode = dir_get_entry_mode(dir, file_name);
    dir_close(dir);
    return mode;
}

int get_file_size(const char* path){
    char* file_name;
    dir_t* dir = follow_path(path, &file_name);
    if(dir == NULL)
        return -1;
    int size = dir_get_entry_size(dir, file_name);
    dir_close(dir);
    return size;
}





int create_file (const char* path, uint64_t mode, file_info_t **f_info){
    char* file_name;
    dir_t* dir = follow_path(path, &file_name);
    inode_t* inode;

    if(dir == NULL)
        return -1;

    if(check_permission(dir->inode, WR)){
        dir_close(dir);
        return -EACCES;
    }
    // readdir_full(dir);
    if(dir_entry_exists(dir, file_name) == 0) {
        inode = inode_open(dir_get_entry_inumber(dir, file_name));
    }else{
        inumber_t inumber = alloc_inumber();
        if(inode_create(inumber, 0, (int)mode) != 0)
            return -1;

        inode = inode_open(inumber);
        if(inode == NULL){
            dir_close(dir);
            return -1;
        }

        int status = dir_add_entry(dir, file_name, inumber);
        if(status < 0){
            dir_close(dir);
            inode_close(inode);
            return -1;
        }

        status = increase_nlink(inode);
        if(status < 0){
            dir_remove_entry(dir, file_name);
            dir_close(dir);
            inode_close(inode);
            return -1;
        }
    }

    if(inode == NULL){
        dir_close(dir);
        return -1;
    }

    file_info_t* fi = malloc(sizeof(file_info_t));
    fi->inode=inode;
    dir_close(dir);
    *f_info = fi;
    return 0;
}

int delete_file (const char* path){
    char* file_name;
    int status = 0;
    dir_t* dir = follow_path(path, &file_name);
    if(dir == NULL)
        return -1;

    if(check_permission(dir->inode, WR)){
        dir_close(dir);
        return -EACCES;
    }

    inumber_t inumber = dir_get_entry_inumber(dir, file_name);
    inode_t* inode = inode_open(inumber);
    if(inode == NULL)
        return -1;

    if(check_permission(inode, WR)){
        dir_close(dir);
        inode_close(inode);
        return -EACCES;
    }

    if(inode->d_inode.nlink == 1){
        status = inode_delete(inode);
    } else {
        status = decrease_nlink(inode);
        inode_close(inode);
    }

    if(status < 0){
        dir_close(dir);
        return -1;
    }

    status = dir_remove_entry(dir, file_name);

    dir_close(dir);
    return status;
}

int filesys_mkdir(const char* path, int mode){
    char *name;
    dir_t* f_dir = follow_path(path, &name);
    if(f_dir == NULL)
        return -1;

    if(check_permission(f_dir->inode, WR)){
        dir_close(f_dir);
        return -EACCES;
    }

    inumber_t inumber = alloc_inumber();
    int status = dir_create(inumber, mode);
    if (status < 0){
        printf("couldn't create dir \n");
        return -1;
    }
    status = dir_add_entry(f_dir, name, inumber);
    dir_t* dir = dir_open(f_dir, name);
    dir_add_entry(dir, ".", inumber);
    dir_add_entry(dir, "..", f_dir->inode->inumber);
    dir_close(dir);
    dir_close(f_dir);
    return status;
}

int filesys_chdir(const char* path){
    char* dir_name;
    dir_t* dir = follow_path(path, &dir_name);
    if(dir == NULL)
        return -1;

    if(!S_ISDIR(dir_get_entry_mode(dir, dir_name))){
        dir_close(dir);
        return -1;
    }

    dir_t* res_dir = dir_open(dir, dir_name);
    if(res_dir == NULL){
        dir_close(dir);
        return -1;
    }
    // TODO: root dir closing needs testing
    dir_close(cur_dir);
    dir_close(dir);
    cur_dir = res_dir;
    return 0;
}

dir_t* filesys_opendir(const char* path){
    if(strcmp(path, "/") == 0)
        return dir_open_root();
    char* dir_name;
    dir_t* dir = follow_path(path, &dir_name);
    if(dir == NULL)
        return NULL;

    if(!S_ISDIR(dir_get_entry_mode(dir, dir_name))){
        dir_close(dir);
        return NULL;
    }

    dir_t* res_dir = dir_open(dir, dir_name);
    dir_close(dir);
    return res_dir;
}

int filesys_rmdir(const char* path){
    if(strcmp(path, "/") == 0)
        return -1;
    char* dir_name;
    dir_t* dir = follow_path(path, &dir_name);
    if(dir == NULL){
        return -1;
    }
    if(check_permission(dir->inode, WR)){
        dir_close(dir);
        return -EACCES;
    }
    if(!S_ISDIR(dir_get_entry_mode(dir, dir_name))){
        dir_close(dir);
        return -1;
    }
    dir_t* dir_to_remove = dir_open(dir, dir_name);
    if(dir_to_remove == NULL){
        dir_close(dir);
        return -1;
    }

     if(check_permission(dir_to_remove->inode, WR)){
        dir_close(dir);
        dir_close(dir_to_remove);
        return -EACCES;
    }

    int status = dir_remove(dir_to_remove);
    if(status < 0){
        dir_close(dir_to_remove);
        dir_close(dir);
        return status;
    }
    status = dir_remove_entry(dir, dir_name);
    dir_close(dir);
    return status;
}

char* get_last_part(const char* path){
    if(path == NULL)
        return NULL;
    int length = (int)strlen(path);
    if(length == 0)
        return NULL;

    char* res;

    length -= 1;
    while(path + length >= path){
        if(path[length] == '/')
            return (char*)path + length;
        length -= 1;
    }
    return NULL;
}


int check_permission(inode_t* inode, char* perms){
    int perm = 0;

    if(getuid() == inode->d_inode.uid){
        printf("usr\n");
        if(perms[0] == 'r')
            perm |= S_IRUSR;
        if(perms[1] == 'w')
            perm |= S_IWUSR;
        if(perms[2] == 'x')
            perm |= S_IXUSR;
        printf("%d\n", perm);
        return (perm & inode->d_inode.mode) == 0;
    }

    if(getgid() == inode->d_inode.gid){
        printf("grp\n");
        if(perms[0] == 'r')
            perm |= S_IRGRP;
        if(perms[1] == 'w')
            perm |= S_IWGRP;
        if(perms[2] == 'x')
            perm |= S_IXGRP;
        return (perm & inode->d_inode.mode) == 0;
    }

    printf("oth\n");
    
    if(perms[0] == 'r')
        perm |= S_IROTH;
    if(perms[1] == 'w')
        perm |= S_IWOTH;
    if(perms[2] == 'x')
        perm |= S_IXOTH;
    return (perm & inode->d_inode.mode) == 0;
}