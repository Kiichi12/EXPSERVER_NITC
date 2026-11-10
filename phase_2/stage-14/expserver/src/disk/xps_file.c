#include "xps_file.h"

void file_source_handler(void *ptr);
void file_source_close_handler(void *ptr);


xps_file_t *xps_file_create(xps_core_t *core, const char *file_path, int *error) {
    assert(core != NULL);
    assert(file_path != NULL);
    assert(error != NULL);

    *error = E_FAIL;
    /*check if file is inside the public directory*/

    char *resolved_path = realpath(file_path, NULL);
    // Resolve the public directory
    char *resolved_public = realpath(DEFAULT_PUBLIC_DIR, NULL);


    if (resolved_path == NULL || resolved_public == NULL) {
        logger(LOG_ERROR, "xps_file_create()", "realpath() failed");
        /*free both path*/
        free(resolved_path);
        free(resolved_public);
        /*close file object*/
        // fclose(file_path);
        return NULL;
    }




    size_t public_len = strlen(resolved_public);
    if (strncmp(resolved_path, resolved_public, public_len) != 0) {
        logger(LOG_WARNING, "xps_file_create()", "file requested is outside of public directory");
        *error = E_PERMISSION;
        /*free both path*/
        free(resolved_path);
        free(resolved_public);
        /*close file object*/

        return NULL;
    }

    /*free both path*/
    // free(resolved_path);
    free(resolved_public);

    
    struct stat file_stat;
    if (stat(file_path, &file_stat) != 0) {
        logger(LOG_ERROR, "xps_file_create()", "stat() failed");
        perror("Error message");
        /*close file object*/
        return NULL;
    }
    // Others must have read permission
    if (!(file_stat.st_mode & S_IROTH)) {
        logger(LOG_WARNING, "xps_file_create()", "others do not have read permission");
        *error = E_PERMISSION;
        return NULL;
    }

    // Getting size of file from stat (already called above)
    long temp_size = file_stat.st_size;

    // Opening file
    FILE *file_struct = fopen(resolved_path, "rb");
    if (file_struct == NULL) {
        int saved_errno = errno;
        if (saved_errno == EACCES) {
            *error = E_PERMISSION;
            logger(LOG_WARNING, "xps_file_create()", "permission denied: %s", resolved_path);
        }
        else if (saved_errno == ENOENT) {
            *error = E_NOTFOUND;
            logger(LOG_WARNING, "xps_file_create()", "file not found: %s", resolved_path);
        }
        else {
            logger(LOG_ERROR, "xps_file_create()", "fopen() failed: %s", strerror(saved_errno));
        }
        free(resolved_path);
        return NULL;
    }

    const char *mime_type = xps_get_mime(resolved_path);

    /*Alloc memory for instance of xps_file_t*/
    xps_file_t *file = malloc(sizeof(xps_file_t));
    if (file == NULL) {
        logger(LOG_ERROR, "xps_file_create()", "malloc() failed for 'file'");
        fclose(file_struct);
        free(resolved_path);
        return NULL;
    }

    // Init values
    
    /*initialise the fields of file instance*/
    file->core = core;
    file->file_path = resolved_path;
    
    file->file_struct = file_struct;
    file->size = (size_t)file_stat.st_size;
    file->mime_type = mime_type;
    xps_pipe_source_t *source = xps_pipe_source_create((void *)file, file_source_handler, file_source_close_handler);
    if (source == NULL) {
        logger(LOG_ERROR, "xps_file_create()", "xps_pipe_source_create() failed");
        fclose(file_struct);
        free(resolved_path);
        free(file_struct);
        return NULL;
    }
    source->ready = true;
    file->source = source;

    

    *error = OK;
    free(resolved_path);
    logger(LOG_DEBUG, "xps_file_create()", "created file");

    return file;
}

void xps_file_destroy(xps_file_t *file) {
    assert(file != NULL);

    if (file->file_struct != NULL)
        fclose(file->file_struct);

    // Detaches the source from its pipe (pipe->source = NULL) and frees it
    if (file->source != NULL)
        xps_pipe_source_destroy(file->source);

    // free((void *)file->file_path);
    free(file);

    logger(LOG_DEBUG, "xps_file_destroy()", "destroyed file struct");
}


void file_source_handler(void *ptr) {
    assert(ptr != NULL);

    xps_pipe_source_t *source = ptr;
    /*get file from source ptr*/
    xps_file_t *file = source->ptr;
    xps_pipe_t *pipe = source->pipe;

    /*create buffer and handle any error*/
    xps_buffer_t *buff = xps_buffer_create(1024, 0, NULL);
    if (buff == NULL) {
        logger(LOG_ERROR, "file_source_handler()", "xps_buffer_create() failed");
        return;
    }

    // Read from file
    size_t read_n = fread(buff->data, 1, buff->size, file->file_struct);
    buff->len = read_n;

    // Checking for read errors
    if (ferror(file->file_struct)) {
        logger(LOG_ERROR, "file_source_handler()", "fread() failed");
        xps_buffer_destroy(buff);
        xps_file_destroy(file);
        return;
    }

    // If end of file reached
    if (read_n == 0 && feof(file->file_struct)) {
        xps_buffer_destroy(buff);
        xps_file_destroy(file);
        return;
    }

    // Write to pipe (xps_pipe_source_write() copies the buffer)
    if (xps_pipe_source_write(source, buff) != OK) {
        logger(LOG_ERROR, "file_source_handler()", "xps_pipe_source_write() failed");
        xps_buffer_destroy(buff);
        xps_file_destroy(file);
        return;
    }

    xps_buffer_destroy(buff);
}


void file_source_close_handler(void *ptr) {
    assert(ptr != NULL);

    xps_pipe_source_t *source = ptr;
    xps_file_t *file = source->ptr;

    xps_file_destroy(file);
}