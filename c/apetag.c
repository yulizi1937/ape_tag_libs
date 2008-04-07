#include "apetag.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Macros */

#define INIT_DBT    DBT key_dbt, value_dbt; \
    key_dbt.data = NULL; \
    key_dbt.size = 0; \
    value_dbt.data = NULL; \
    value_dbt.size = 0;

/* Determine endianness */
#ifndef IS_BIG_ENDIAN
#ifdef _BYTE_ORDER
#ifdef _BIG_ENDIAN
#if _BYTE_ORDER == _BIG_ENDIAN
#define IS_BIG_ENDIAN 1
#endif
#endif
#else
#ifdef __BYTE_ORDER
#ifdef __BIG_ENDIAN
#if __BYTE_ORDER == __BIG_ENDIAN
#define IS_BIG_ENDIAN 1
#endif
#endif
#endif
#endif
#endif
/* From OpenBSD */
#ifdef IS_BIG_ENDIAN
#define SWAPEND32(x) \
    (u_int32_t)(((u_int32_t)(x) & 0xff) << 24 | \
    ((u_int32_t)(x) & 0xff00) << 8 | \
    ((u_int32_t)(x) & 0xff0000) >> 8 | \
    ((u_int32_t)(x) & 0xff000000) >> 24)
#define H2LE32(X) SWAPEND32(X) 
#define LE2H32(X) SWAPEND32(X) 
#else
#define H2LE32(X) (X)
#define LE2H32(X) (X)
#endif

/* Global Variables */

DB* ID3_GENRES = NULL;

/* Public Functions */

ApeTag* ApeTag_new(FILE *file, u_int32_t flags) {
    ApeTag* tag;
    
    assert(file != NULL);
    
    tag = (ApeTag*)malloc(sizeof(ApeTag));
    if(tag != NULL) {
        tag->file = file;
        tag->fields = NULL;
        tag->id3 = NULL;
        tag->tag_header = NULL;
        tag->tag_footer = NULL;
        tag->tag_data = NULL;
        tag->error = NULL;
        tag->flags = flags | APE_DEFAULT_FLAGS;
        tag->size = 0;
        tag->item_count = 0;
        tag->num_fields = 0;
        tag->offset = 0;
    }
    
    return tag;
}

int ApeTag_free(ApeTag* tag) {
    int ret = 0;
    
    if(tag == NULL) {
        return 0;
    }
    
    /* Free the information stored in the database */
    if(ApeTag_clear_fields(tag) == -1) {
        ret = -1;
    }
    
    /* Free char* on the heap first, then the tag itself */
    free(tag->id3);
    tag->id3 = NULL;
    free(tag->tag_header);
    tag->tag_header = NULL;
    free(tag->tag_footer);
    tag->tag_footer = NULL;
    free(tag->tag_data);
    tag->tag_data = NULL;
    free(tag);
    tag = NULL;
    
    return ret;
}

int ApeTag_exists(ApeTag* tag) {
    int ret = 0;
    
    assert(tag != NULL);
    
    if(!(tag->flags & APE_CHECKED_APE)) {
        if((ret = ApeTag__get_tag_information(tag)) < 0) {
            return ret;
        }
    }
    
    return (tag->flags & APE_HAS_APE) > 0;
}

int ApeTag_remove(ApeTag* tag) {
    int ret = 0;
    
    assert(tag != NULL);
    
    if(!(tag->flags & APE_CHECKED_APE)) {
        if((ret = ApeTag__get_tag_information(tag)) < 0) {
            return ret;
        }
    }
    
    if(!(tag->flags & APE_HAS_APE)) {
        return 1;
    }
    if((ret = ftruncate(fileno(tag->file), (off_t)tag->offset)) == -1) {
        tag->error = "ftruncate";
        return ret;
    }
    
    tag->flags &= ~(APE_HAS_APE|APE_HAS_ID3);
    return 0;
}

int ApeTag_raw(ApeTag* tag, char** raw) {    
    int ret = 0;
    u_int32_t raw_size; 
    
    assert(tag != NULL);
    assert(raw != NULL);
    
    if(!(tag->flags & APE_CHECKED_APE)) {
        if((ret = ApeTag__get_tag_information(tag)) < 0) {
            return ret;
        }
    }
    
    raw_size = TAG_LENGTH(tag);
    if((*raw = (char *)malloc(raw_size)) == NULL) {
        tag->error = "malloc";
        return -1;
    }
    if(tag->flags & APE_HAS_APE) {
        bcopy(tag->tag_header, *raw, 32);
        bcopy(tag->tag_data, *raw+32, tag->size-64);
        bcopy(tag->tag_footer, *raw+tag->size-32, 32);
    }
    if(tag->flags & APE_HAS_ID3 && !(tag->flags & APE_NO_ID3)) {
        bcopy(tag->id3, *raw+tag->size, ID3_LENGTH(tag));
    }
    
    return 0;
}

int ApeTag_parse(ApeTag* tag) {
    int ret = 0;
    
    assert(tag != NULL);
    
    if(!(tag->flags & APE_CHECKED_APE)) {
        if((ret = ApeTag__get_tag_information(tag)) < 0) {
            return ret;
        }
    }
    if((tag->flags & APE_HAS_APE) && !(tag->flags & APE_CHECKED_FIELDS)) {
        if((ret = ApeTag__parse_fields(tag)) < 0) {
            return ret;
        }
    }
    
    return 0;
}

int ApeTag_update(ApeTag* tag) {
    int ret = 0;
    
    assert(tag != NULL);
    
    if(!(tag->flags & APE_CHECKED_APE)) {
        if((ret = ApeTag__get_tag_information(tag)) < 0) {
            return ret;
        }
    }
    
    if((ret = ApeTag__update_id3(tag)) != 0) {
        return ret;
    }
    if((ret = ApeTag__update_ape(tag)) != 0) {
        return ret;
    }
    if((ret = ApeTag__write_tag(tag)) != 0) {
        return ret;
    }
    
    return 0;
}

int ApeTag_add_field(ApeTag *tag, ApeItem *item) {
    int ret = 0;
    INIT_DBT;
    value_dbt.size = sizeof(ApeItem **);
    value_dbt.data = &item; 
    key_dbt.size = strlen(item->key)+1;
    
    assert(tag != NULL);
    assert(item != NULL);
    assert(item->key != NULL);
    assert(item->value != NULL);

    if(!(tag->flags & APE_CHECKED_APE)) {
        if((ret = ApeTag__get_tag_information(tag)) < 0) {
            return ret;
        }
    }
    
    /* Don't add invalid items to the database */
    if(ApeItem__check_validity(tag, item) != 0) {
        return -3;
    }
    
    /* Don't exceed the maximum number of items allowed */
    if(tag->num_fields == APE_MAXIMUM_ITEM_COUNT) {
        tag->error = "maximum item count exceeded";
        return -3;
    }
    
    /* Create the database if it doesn't already exist */
    if(tag->fields == NULL) {
        if((tag->fields = dbopen(NULL, O_RDWR, 0777, DB_HASH, NULL)) == NULL) {
            tag->error = "dbopen";
            return -1;
        }
    }
    
    /* Apetag keys are case insensitive but case preserving */
    if((key_dbt.data = ApeTag__strcasecpy(item->key, (unsigned char)key_dbt.size)) == NULL) {
        tag->error = "malloc";
        ret = -1;
        goto add_field_error;
    }
    
    /* Add to the database */
    ret = tag->fields->put(tag->fields, &key_dbt, &value_dbt, R_NOOVERWRITE);
    if(ret == -1) {
        tag->error = "db->put";
        goto add_field_error;
    } else if(ret == 1) {
        tag->error = "duplicate field in tag";
        ret = -3;
        goto add_field_error;
    }

    tag->num_fields++;
    ret = 0;
    
    add_field_error:
    free(key_dbt.data);
    return ret;
}

int ApeTag_remove_field(ApeTag* tag, char* key) {
    int ret = 0;
    INIT_DBT;
    key_dbt.size = strlen(key) + 1;
    
    assert(tag != NULL);
    assert(key != NULL);
    
    /* Empty database implies field doesn't exist */
    if(tag->fields == NULL) {
        return 1;
    }
    
    /* Apetag keys are case insensitive but case preserving */
    if((key_dbt.data = ApeTag__strcasecpy(key, (unsigned char)key_dbt.size)) == NULL)  {
        tag->error = "malloc";
        return -1;
    }
    
    /* Get the item from the database so it can be freed */
    ret = tag->fields->get(tag->fields, &key_dbt, &value_dbt, 0);
    if(ret != 0) {
        if(ret == -1) {
            tag->error = "db->get";
        }
        free(key_dbt.data);
        return ret;
    }

    /* Free the item and remove it from the database  */
    ApeItem__free((ApeItem **)(value_dbt.data));
    ret = tag->fields->del(tag->fields, &key_dbt, 0);
    if(ret != 0) {
        if(ret == -1) {
            tag->error = "db->del";
        } else if(ret == 1) {
            tag->error = "database modified between get and del";
            ret = -3;
        }
    }
    
    tag->num_fields--;
    free(key_dbt.data);
    return ret;
}

int ApeTag_clear_fields(ApeTag* tag) {
    int ret = 0;
    INIT_DBT;
    
    assert(tag != NULL);
    
    if(!(tag->flags & APE_CHECKED_APE)) {
        if((ret = ApeTag__get_tag_information(tag)) < 0) {
            goto clear_fields_error;
        }
    }
    
    if(tag->fields != NULL) {
        /* Free all items in the database and then close it */
        if(tag->fields->seq(tag->fields, &key_dbt, &value_dbt, R_FIRST) == 0) {
            ApeItem__free((ApeItem **)(value_dbt.data));
            while(tag->fields->seq(tag->fields, &key_dbt, &value_dbt, R_NEXT) == 0) {
                ApeItem__free((ApeItem **)(value_dbt.data));
            }
        }
        if(tag->fields->close(tag->fields) == -1) {
            tag->error = "db->close";
            ret = -1;
            goto clear_fields_error;
        }
    }
    
    ret = 0;
    
    clear_fields_error:
    tag->fields = NULL;
    tag->flags &= ~APE_CHECKED_FIELDS;
    tag->num_fields = 0;
    return ret;
}

/* Private Functions */

/*
Parses the header and footer of the tag to get information about it.

Returns 0 on success, <0 on error;
*/
int ApeTag__get_tag_information(ApeTag* tag) {
    int id3_length = 0;
    u_int32_t header_check;
    long file_size = 0;
    assert(tag != NULL);
    
    /* Get file size */
    if(fseek(tag->file, 0, SEEK_END) == -1) {
        tag->error = "fseek";
        return -1;
    }
    if((file_size = ftell(tag->file)) == -1) {
        tag->error = "ftell";
        return -1;
    } 
    
    /* No ape or id3 tag possible in this size */
    if(file_size < APE_MINIMUM_TAG_SIZE) {
        tag->offset = file_size;
        tag->flags |= APE_CHECKED_APE | APE_CHECKED_OFFSET;
        tag->flags &= ~(APE_HAS_APE | APE_HAS_ID3);
        return 0;
    } 
    
    if(!(tag->flags & APE_NO_ID3)) {
        if(file_size < 128) {
            /* No id3 tag possible in this size */
            tag->flags &= ~APE_HAS_ID3;
        } else {
            /* Check for id3 tag */
            if((fseek(tag->file, -128, SEEK_END)) == -1) {
                tag->error = "fseek";
                return -1;
            }
            free(tag->id3);
            if((tag->id3 = (char *)malloc(128)) == NULL) {
                tag->error = "malloc";
                return -1;
            }
            if(fread(tag->id3, 1, 128, tag->file) < 128) {
                tag->error = "fread";
                return -2;
            }
            if(tag->id3[0] == 'T' && tag->id3[1] == 'A' && 
               tag->id3[2] == 'G' && tag->id3[125] == '\0') {
                id3_length = 128;
                tag->flags |= APE_HAS_ID3;
            } else {
                free(tag->id3);
                tag->id3 = NULL;
                tag->flags &= ~APE_HAS_ID3;
            }
        }
        /* Recheck possibility for ape tag now that id3 presence is known */
        if(file_size < APE_MINIMUM_TAG_SIZE + id3_length) {
            tag->flags &= ~APE_HAS_APE;
            tag->offset = file_size - id3_length;
            tag->flags |= APE_CHECKED_OFFSET | APE_CHECKED_APE;
            return 0;
        }
    }
    
    /* Check for existance of ape tag footer */
    if(fseek(tag->file, -32-id3_length, SEEK_END) == -1) {
        tag->error = "fseek";
        return -1;
    }
    free(tag->tag_footer);
    if((tag->tag_footer = (char *)malloc(32)) == NULL) {
        tag->error = "malloc";
        return -1;
    }
    if(fread(tag->tag_footer, 1, 32, tag->file) < 32) {
        tag->error = "fread";
        return -2;
    }
    if(bcmp(APE_PREAMBLE, tag->tag_footer, 12)) {
        tag->flags &= ~APE_HAS_APE;
        tag->offset = file_size - id3_length;
        tag->flags |= APE_CHECKED_OFFSET | APE_CHECKED_APE;
        return 0;
    }
    if(bcmp(APE_FOOTER_FLAGS, tag->tag_footer+21, 3) || \
       ((char)*(tag->tag_footer+20) != '\0' && \
       (char)*(tag->tag_footer+20) != '\1')) {
        tag->error = "bad tag footer flags";
        return -3;
    }
    
    bcopy(tag->tag_footer+12, &tag->size, 4);
    bcopy(tag->tag_footer+16, &tag->item_count, 4);
    tag->size = LE2H32(tag->size);
    tag->item_count = LE2H32(tag->item_count);
    tag->size += 32;
    
    /* Check tag footer for validity */
    if(tag->size < APE_MINIMUM_TAG_SIZE) {
        tag->error = "tag smaller than minimum possible size";
        return -3;
    }
    if(tag->size > APE_MAXIMUM_TAG_SIZE) {
        tag->error = "tag larger than maximum possible size";
        return -3;
    }
    if(tag->size + (u_int32_t)id3_length > (unsigned long)file_size) {
        tag->error = "tag larger than possible size";
        return -3;
    }
    if(tag->item_count > APE_MAXIMUM_ITEM_COUNT) {
        tag->error = "tag item count larger than allowed";
        return -3;
    }
    if(tag->item_count > (tag->size - APE_MINIMUM_TAG_SIZE)/APE_ITEM_MINIMUM_SIZE) {
        tag->error = "tag item count larger than possible";
        return -3;
    }
    if(fseek(tag->file, (-(long)tag->size - id3_length), SEEK_END) == -1) {
        tag->error = "fseek";
        return -1;
    }
    if((tag->offset = ftell(tag->file)) == -1) {
        tag->error = "ftell";
        return -1;
    }
    tag->flags |= APE_CHECKED_OFFSET;
    
    /* Read tag header and data */
    free(tag->tag_header);
    if((tag->tag_header = (char *)malloc(32)) == NULL) {
        tag->error = "malloc";
        return -1;
    }
    if(fread(tag->tag_header, 1, 32, tag->file) < 32) {
        tag->error = "fread";
        return -2;
    }
    free(tag->tag_data);
    if((tag->tag_data = (char *)malloc(tag->size-64)) == NULL) {
        tag->error = "malloc";
        return -1;
    }
    if(fread(tag->tag_data, 1, tag->size-64, tag->file) < tag->size-64) {
        tag->error = "fread";
        return -2;
    }
    
    /* Check tag header for validity */
    if(bcmp(APE_PREAMBLE, tag->tag_header, 12) || bcmp(APE_HEADER_FLAGS, tag->tag_header+21, 3) \
      || ((char)*(tag->tag_header+20) != '\0' && (char)*(tag->tag_header+20) != '\1')) {
        tag->error = "missing APE header";
        return -3;
    }
    bcopy(tag->tag_header+12, &header_check, 4);
    if(tag->size != LE2H32(header_check)+32) {
        tag->error = "header and footer size does not match";
        return -3;
    }
    bcopy(tag->tag_header+16, &header_check, 4);
    if(tag->item_count != LE2H32(header_check)) {
        tag->error = "header and footer item count does not match";
        return -3;
    }
    
    tag->flags |= APE_CHECKED_APE | APE_HAS_APE;
    return 1;
}

/* 
Parses all fields from the tag and puts them in the database.

Returns 0 on success, <0 on error.
*/
int ApeTag__parse_fields(ApeTag* tag) {
    u_int32_t i;
    u_int32_t offset = 0;
    int ret =0;
    u_int32_t last_possible_offset = tag->size - APE_MINIMUM_TAG_SIZE - 
                               APE_ITEM_MINIMUM_SIZE;
    
    assert(tag != NULL);
    
    if(tag->fields != NULL) {
        if(ApeTag_clear_fields(tag) != 0) {
            return -1;
        }
    }
    
    for(i=0; i < tag->item_count; i++) {
        if(offset > last_possible_offset) {
            tag->error = "end of tag reached but more items specified";
            return -3;
        }

        if((ret = ApeTag__parse_field(tag, &offset)) != 0) {
            return ret;
        }
    }
    if(offset != tag->size - APE_MINIMUM_TAG_SIZE) {
        tag->error = "data remaining after specified number of items parsed";
        return -3;
    }
    tag->flags |= APE_CHECKED_FIELDS;
    
    return 0;
}

/* 
Parses a single field from the tag at the given offset from the start of the
tag's data.

Returns 0 on success, <0 on error.
*/
int ApeTag__parse_field(ApeTag* tag, u_int32_t* offset) {
    char* data = tag->tag_data;
    char* value_start = NULL;
    char* key_start = data+(*offset)+8;
    u_int32_t data_size = tag->size - APE_MINIMUM_TAG_SIZE;
    u_int32_t key_length;
    int ret = 0;
    ApeItem* item = NULL;
    
    assert(tag != NULL);

    if((item = (ApeItem *)malloc(sizeof(ApeItem))) == NULL) {
        tag->error = "malloc";
        return -1;
    }
    
    bcopy(data+(*offset), &item->size, 4);
    bcopy(data+(*offset)+4, &item->flags, 4);
    item->size = LE2H32(item->size);
    item->flags = LE2H32(item->flags);
    item->key = NULL;
    item->value = NULL;
    
    /* Find and check start of value */
    if(item->size + *offset + APE_ITEM_MINIMUM_SIZE > data_size) {
        tag->error = "impossible item length (greater than remaining space)";
        ret = -3;
        goto parse_error;
    }
    for(value_start=key_start; value_start < key_start+256 && \
        *value_start != '\0'; value_start++) {
        /* Left Blank */
    }
    if(*value_start != '\0') {
        tag->error = "invalid item key length (too long or no end)";
        ret = -3;
        goto parse_error;
    }
    value_start++;
    key_length = (u_int32_t)(value_start - key_start);
    *offset += 8 + key_length + item->size;
    if(*offset > data_size) {
        tag->error = "invalid item length (longer than remaining data)";
        ret = -3;
        goto parse_error;
    }
    
    /* Copy key and value from tag data to item */
    if((item->key = (char *)malloc(key_length)) == NULL) {
        tag->error = "malloc";
        ret = -1;
        goto parse_error;
    }
    if((item->value = (char *)malloc(item->size)) == NULL) {
        tag->error = "malloc";
        ret = -1;
        goto parse_error;
    }
    bcopy(key_start, item->key, key_length);
    bcopy(value_start, item->value, item->size);
    
    /* Add item to the database */
    if((ret = ApeTag_add_field(tag, item)) != 0) {
        goto parse_error;
    }

    return 0;
    
    parse_error:
    free(item->key);
    free(item->value);
    free(item);
    return ret;
}

/* 
Updates the id3 tag using the new ape tag values.  Does not merge it with a
previous id3 tag, it overwrites it completely.

Returns 0 on success, <0 on error.
*/
int ApeTag__update_id3(ApeTag* tag) {
    ApeItem* item;
    char* c;
    char* end;
    int ret = 0;
    u_int32_t size;
    INIT_DBT;
    
    assert (tag != NULL);
    
    free(tag->id3);
    
    if(tag->flags & APE_NO_ID3 || 
       (tag->flags & APE_HAS_APE && !(tag->flags & APE_HAS_ID3))) {
        tag->id3 = NULL;
        return 0;
    }
    
    /* Initialize id3 */
    if((tag->id3 = (char *)malloc(128)) == NULL) {
        tag->error = "malloc";
        return -1;
    }
    bcopy("TAG", tag->id3, 3);
    bzero(tag->id3+3, 124);
    *(tag->id3+127) = '\377';
    
    if(tag->fields == NULL) {
        return 0;
    }

    /* Easier to use a macro than a function in this case */
    #define APE_FIELD_TO_ID3_FIELD(FIELD, FIELD_LENGTH, LENGTH, OFFSET) \
        key_dbt.data = #FIELD; \
        key_dbt.size = FIELD_LENGTH; \
        ret = tag->fields->get(tag->fields, &key_dbt, &value_dbt, 0); \
        if(ret == -1) { \
            tag->error = "db->get"; \
            return -1; \
        } else if(ret == 0) { \
            item = *(ApeItem **)(value_dbt.data); \
            size = (item->size < LENGTH ? item->size : LENGTH); \
            end = tag->id3 + OFFSET + size; \
            bcopy(item->value, tag->id3 + OFFSET, size); \
            for(c=tag->id3 + OFFSET; c < end; c++) { \
                if(*c == '\0') { \
                    *c = ','; \
                } \
            } \
        } \
    
    /* 
    ID3v1.1 tag offsets, lengths
    title - 3, 30
    artist - 33, 30
    album - 63, 30
    year - 93, 4
    comment - 97, 28
    track - 126, 1
    genre - 127, 1
    */
    APE_FIELD_TO_ID3_FIELD(title, 6, 30, (u_int32_t)3);
    APE_FIELD_TO_ID3_FIELD(artist, 7, 30, (u_int32_t)33);
    APE_FIELD_TO_ID3_FIELD(album, 6, 30, (u_int32_t)63);
    APE_FIELD_TO_ID3_FIELD(year, 5, 4, (u_int32_t)93);
    APE_FIELD_TO_ID3_FIELD(comment, 8, 28, (u_int32_t)97);
    
    #undef APE_FIELD_TO_ID3_FIELD
    
    /* Need to handle the track and genre differently, as they are just bytes */
    key_dbt.data = "track"; 
    key_dbt.size = 6; 
    ret = tag->fields->get(tag->fields, &key_dbt, &value_dbt, 0);
    if(ret == -1) { 
        tag->error = "db->get"; 
        return -1; 
    } else if(ret == 0) { 
        item = *(ApeItem **)(value_dbt.data);
        *(tag->id3+126) = (char)ApeItem__parse_track(item->size, item->value);
    } 
    key_dbt.data = "genre"; 
    key_dbt.size = 6; 
    ret = tag->fields->get(tag->fields, &key_dbt, &value_dbt, 0);
    if(ret == -1) { 
        tag->error = "db->get"; 
        return -1; 
    } else if(ret == 0) { 
        value_dbt.data = (*(ApeItem **)(value_dbt.data))->value;
        if(ApeTag__lookup_genre(tag, &value_dbt, tag->id3+127) != 0) {
            return -1;
        }
    } 
    
    return 0;
}

/* 
Updates the internal ape tag strings using the value for the database.

Returns 0 on success, <0 on error.
*/
int ApeTag__update_ape(ApeTag* tag) {
    u_int32_t i = 0;
    u_int32_t key_size;
    char* c;
    int ret = 0;
    u_int32_t size;
    u_int32_t flags;
    u_int32_t tag_size = 64 + 9 * tag->num_fields;
    u_int32_t num_fields = tag->num_fields;
    ApeItem** items;
    INIT_DBT;
    
    assert(tag != NULL);
    
    /* Check that the total number of items in the tag is ok */
    if(num_fields > APE_MAXIMUM_ITEM_COUNT) {
        tag->error = "tag item count larger than allowed";
        return -3;
    }
    
    if((items = (ApeItem **)calloc(num_fields, sizeof(ApeItem *))) == NULL) {
        tag->error = "calloc";
        return -1;
    }
    
    /* Get all ape items from the database */
    if(tag->fields != NULL) {
        if(tag->fields->seq(tag->fields, &key_dbt, &value_dbt, R_FIRST) == 0) {
            if(num_fields == 0) {
                tag->error = "num_fields is zero but fields in database";
                ret = -3;
                goto update_ape_error;
            }
            items[i++] = *(ApeItem **)(value_dbt.data);
            while(tag->fields->seq(tag->fields, &key_dbt, &value_dbt, R_NEXT) == 0) {
                if(i>=num_fields) {
                    tag->error = "num_fields is less than fields in database";
                    ret = -3;
                    goto update_ape_error;
                }
                items[i++] = *(ApeItem **)(value_dbt.data);
            }
        }
        if(i != num_fields) {
            tag->error = "num_fields is greater than fields in database";
            ret = -3;
            goto update_ape_error;
        }
         /* Sort them */
        qsort(items, num_fields, sizeof(ApeItem *), ApeItem__compare);
    } else if(num_fields != 0) {
        tag->error = "num_fields is not zero but database is empty";
        ret = -3;
        goto update_ape_error;
    }
    
    /* Check all of them for validity and update the total size of the tag*/
    for(i=0; i < num_fields; i++) {
        if(ApeItem__check_validity(tag, items[i]) != 0) {
            ret = -3;
            goto update_ape_error;
        }
        tag_size += items[i]->size + (u_int32_t)strlen(items[i]->key);
    }
    
    /* Check that the total size of the tag is ok */
    tag->size = tag_size;
    if(tag->size > APE_MAXIMUM_TAG_SIZE) {
        tag->error = "tag larger than maximum possible size";
        ret = -3;
        goto update_ape_error;
    }
    
    /* Write all of the tag items to the internal tag item string */
    free(tag->tag_data);
    if((tag->tag_data = (char *)malloc(tag->size-64)) == NULL) {
        tag->error = "malloc";
        ret = -1;
        goto update_ape_error;
    }
    for(i=0, c=tag->tag_data; i < num_fields; i++) {
        key_size = (u_int32_t)strlen(items[i]->key) + 1;
        size = H2LE32(items[i]->size);
        flags = H2LE32(items[i]->flags);
        bcopy(&size, c, 4);
        bcopy(&flags, c+=4, 4);
        bcopy(items[i]->key, c+=4, key_size);
        bcopy(items[i]->value, c+=key_size, items[i]->size);
        c += items[i]->size;
    }
    if((u_int32_t)(c - tag->tag_data) != tag_size - 64) {
        tag->error = "internal inconsistancy in creating new tag data";
        ret = -3;
        goto update_ape_error;
    }
    
    free(tag->tag_footer);
    if((tag->tag_footer = (char *)malloc(32)) == NULL) {
        tag->error = "malloc";
        ret = -1;
        goto update_ape_error;
    }
    free(tag->tag_header);
    if((tag->tag_header = (char *)malloc(32)) == NULL) {
        tag->error = "malloc";
        ret = -1;
        goto update_ape_error;
    }
    
    /* Update the internal tag header and footer strings */
    tag_size = H2LE32(tag_size - 32);
    num_fields = H2LE32(num_fields);
    bcopy(APE_PREAMBLE, tag->tag_header, 12);
    bcopy(APE_PREAMBLE, tag->tag_footer, 12);
    bcopy(&tag_size, tag->tag_header+12, 4);
    bcopy(&tag_size, tag->tag_footer+12, 4);
    bcopy(&num_fields, tag->tag_header+16, 4);
    bcopy(&num_fields, tag->tag_footer+16, 4);
    *(tag->tag_header+20) = '\0';
    *(tag->tag_footer+20) = '\0';
    bcopy(APE_HEADER_FLAGS, tag->tag_header+21, 4);
    bcopy(APE_FOOTER_FLAGS, tag->tag_footer+21, 4);
    bzero(tag->tag_header+24, 8);
    bzero(tag->tag_footer+24, 8);
    
    ret = 0;
    
    update_ape_error:
    free(items);
    return ret;
}

/* 
Writes the tag to the file using the internal tag strings.

Returns 0 on success, <0 on error.
*/
int ApeTag__write_tag(ApeTag* tag) {
    assert(tag != NULL);
    assert(tag->tag_header != NULL);
    assert(tag->tag_data != NULL);
    assert(tag->tag_footer != NULL);
    
    if(fseek(tag->file, tag->offset, SEEK_SET) == -1) {
        tag->error = "fseek";
        return -1;
    }
    
    if(fwrite(tag->tag_header, 1, 32, tag->file) != 32) {
        tag->error = "fwrite";
        return -2;
    }
    if(fwrite(tag->tag_data, 1, tag->size-64, tag->file) != tag->size-64) {
        tag->error = "fwrite";
        return -2;
    }
    if(fwrite(tag->tag_footer, 1, 32, tag->file) != 32) {
        tag->error = "fwrite";
        return -2;
    }
    if(tag->id3 != NULL && !(tag->flags & APE_NO_ID3)) {
        if(fwrite(tag->id3, 1, 128, tag->file) != 128) {
            tag->error = "fwrite";
            return -2;
        }
        tag->flags |= APE_HAS_ID3;
    }

    if(ftruncate(fileno(tag->file), (off_t)((unsigned long)tag->offset + TAG_LENGTH(tag))) == -1) {
        tag->error = "ftruncate";
        return -1;
    }
    tag->item_count = tag->num_fields;
    tag->flags |= APE_HAS_APE;
    
    return 0;
}

/*
Frees an ApeItem and it's key and value, given a pointer to a pointer to it.
*/
void ApeItem__free(ApeItem** item) {
    assert(item != NULL);
    if(*item == NULL) {
        return;
    }
    
    free((*item)->key);
    (*item)->key = NULL;
    free((*item)->value);
    (*item)->value = NULL;
    free(*item);
    *item = NULL;
}

/*
Allocates space for a copy of src, copies src to the newly allocated space
(for the given number of bytes), and converts the copy to lower case.  Size is
an unsigned char because keys cannot be more than 256 bytes (including
terminator).

The caller is responsible for freeing the returned pointer.

Returns pointer to copy on success, NULL pointer on error.
*/
char* ApeTag__strcasecpy(char* src, unsigned char size) {
    unsigned char i;
    char* dest;
    
    assert(src != NULL);
    
    if((dest = (char *)malloc(size)) == NULL) {
        return NULL;
    }
    
    bcopy(src, dest, (unsigned long)size);
    for(i=0; i < size; i++) {
        if(*(dest+i) >= 'A' && *(dest+i) <= 'Z') {
            *(dest+i) |= 0x20;
        }
    } 
    
    return dest;
}

/*
This is a very simple atoi-like function that takes the size of the string, and
a character pointer.  If the character pointer is a string between "0" and
"255", the unsigned char equivalent is returned; otherwise, 0 is returned.

Returns unsigned char.
*/
unsigned char ApeItem__parse_track(u_int32_t size, char* value) {
    assert(value != NULL);
    
    if(size != 0 && size < 4) {
        if(size == 3) {
            if(*(value) >= '0' && *(value) <= '2' && 
               *(value+1) >= '0' && *(value+1) <= '9' &&
               *(value+2) >= '0' && *(value+2) <= '9') {
                if(*(value) == '2' && ((*(value+1) > '5') ||
                   (*(value+1) == '5' && *(value+2) > '5'))) {
                    /* 3 digit number > 255 won't fit in a char */
                    return 0;
                }
                return (unsigned char)(100 * (*(value) & ~0x30) + 
                       (10 * (*(value+1) & ~0x30)) + 
                       (*(value+2) & ~0x30));                        
            }
        } else if(size == 2) {
            if(*(value) >= '0' && *(value) <= '9' && 
               *(value+1) >= '0' && *(value+1) <= '9') {
                return (unsigned char)(10 * (*(value) & ~0x30) + 
                       (*(value+1) & ~0x30));
            }
        } else if(size == 1) {
            if(*(value) >= '0' && *(value) <= '9') {
                 return (unsigned char)(*(value) & ~0x30);
            }
        }
    }
    return 0;
}

/*
Checks the given ApeItem for validity (checking flags, key, and value).

Returns 0 if valid, <0 otherwise.
*/
int ApeItem__check_validity(ApeTag* tag, ApeItem* item) {
    unsigned long key_length;
    char* key_end;
    char* c;
    
    assert(tag != NULL);
    assert(item != NULL);
    assert(item->key != NULL);
    assert(item->value != NULL);
    
    /* Check valid flags */
    if(item->flags > 7) {
        tag->error = "invalid item flags";
        return -3;
    }
    
    /* Check valid key */
    key_length = strlen(item->key);
    key_end = item->key + (long)key_length;
    if(key_length < 2) {
        tag->error = "invalid item key (too short)";
        return -3;
    }
    if(key_length > 255) {
        tag->error = "invalid item key (too long)";
        return -3;
    }
    if(key_length == 3 ? strncasecmp(item->key, "id3", 3) == 0 || 
                         strncasecmp(item->key, "tag", 3) == 0 || 
                         strncasecmp(item->key, "mp+", 3) == 0
       : (key_length == 4 ? strncasecmp(item->key, "oggs", 4) == 0 : 0)) {
        tag->error = "invalid item key (id3|tag|mp+|oggs)";
        return -3;
    }
    for(c=item->key; c<key_end; c++) {
        if((unsigned char)(*c) < 0x20 || (unsigned char)(*c) > 0x7f) {
            tag->error = "invalid item key character";
            return -3;
        }
    }
    
    /* Check value is utf-8 if flags specify utf8 or external format*/
    if(((item->flags & APE_ITEM_TYPE_FLAGS) & 2) == 0 && 
        ApeTag__check_valid_utf8((unsigned char*)(item->value), item->size) != 0) {
        tag->error = "invalid utf8 value";
        return -3;
    }
    
    return 0;
}

/*
Checks the given UTF8 string for validity.

Returns 0 if valid, -1 if not.
*/
int ApeTag__check_valid_utf8(unsigned char* utf8_string, u_int32_t size) {
    unsigned char* utf_last_char;
    unsigned char* c = utf8_string;
    
    assert(utf8_string != NULL);
    
    for(; c < (utf8_string + size); c++) {
        if((*c & 128) != 0) {
            /* Non ASCII */
            if((*c < 194) || (*c > 245)) {
                /* Outside of UTF8 Range */
                return -1;
            }
            if((*c & 224) == 192) {
                /* 2 byte UTF8 char */
                utf_last_char = c + 1;
            } else if((*c & 240) == 224) {
                /* 3 byte UTF8 char */
                utf_last_char = c + 2;
            } else if((*c & 248) == 240) {
                /* 4 byte UTF8 char */
                utf_last_char = c + 3;
            } else {
                return -1;
            }
            
            if(utf_last_char >= (utf8_string + size)) {
                return -1;
            }
            /* Check remaining bytes of character */
            for(c++; c <= utf_last_char; c++) {
                if((*c & 192) != 128) {
                    return -1;
                }
            }
        }
    }
    return 0;
}

/* 
Comparison function for quicksort.  Sorts first based on size and secondly
based on key.  Should be a stable sort, as no two items should have the same
key.

Returns -1 or 1.  Could possibly return 0 if the database has been manually
modified (don't do that!).
*/
int ApeItem__compare(const void* a, const void* b) {
    const ApeItem* ai_a = *(const ApeItem_CP*)a;
    const ApeItem* ai_b = *(const ApeItem_CP*)b;
    u_int32_t size_a;
    u_int32_t size_b;
    
    size_a = ai_a->size + (u_int32_t)strlen(ai_a->key);
    size_b = ai_b->size + (u_int32_t)strlen(ai_b->key);
    if(size_a < size_b) {
        return -1;
    }
    if(size_a > size_b) {
        return 1;
    }
    return strncmp(ai_a->key, ai_b->key, strlen(ai_a->key));
}

/*
Looks up a genre for the correct ID3 genre code.  The genre string is passed
in a DBT, and pointer to the genre code is passed.  The genre string should not
be null terminated, as the entries in the genre database are not.

Returns 0 on success, -1 on error;
*/
int ApeTag__lookup_genre(ApeTag* tag, DBT* key_dbt, char* genre_id) {
    int ret = 0;
    DBT value_dbt;
    value_dbt.data = NULL;
    
    assert(tag != NULL);
    
    if(ApeTag__load_ID3_GENRES(tag) != 0) {
        return -1;
    }
    
    ret = ID3_GENRES->get(ID3_GENRES, key_dbt, &value_dbt, 0);
    if(ret != 0) {
        if(ret == -1) {
            tag->error = "db->get";
            return -1;
        }
        *genre_id = '\255';
    } else {
        *genre_id = *(char*)(value_dbt.data);
    }
    
    return 0;
}

/*
Loads the ID3_GENRES global database with all 148 ID3 genres (including the 
Winamp extensions).

Returns 0 on success, -1 on error.
*/
int ApeTag__load_ID3_GENRES(ApeTag* tag) {
    INIT_DBT;
    value_dbt.size = 1;
    
    assert(tag != NULL);
    
    if(ID3_GENRES != NULL) {
        return 0;
    }
    if((ID3_GENRES = dbopen(NULL, O_RDWR, 0777, DB_HASH, NULL)) == NULL) {
        tag->error = "dbopen";
        return -1;
    }
    
    #define ADD_TO_ID3_GENRES(GENRE, LENGTH, VALUE) \
        key_dbt.data = GENRE; \
        key_dbt.size = LENGTH; \
        value_dbt.data = VALUE; \
        if(ID3_GENRES->put(ID3_GENRES, &key_dbt, &value_dbt, 0) == -1) { \
            tag->error = "db->put"; \
            goto load_genres_error; \
        }

    ADD_TO_ID3_GENRES("Blues", 5, "\0");
    ADD_TO_ID3_GENRES("Classic Rock", 12, "\1");
    ADD_TO_ID3_GENRES("Country", 7, "\2");
    ADD_TO_ID3_GENRES("Dance", 5, "\3");
    ADD_TO_ID3_GENRES("Disco", 5, "\4");
    ADD_TO_ID3_GENRES("Funk", 4, "\5");
    ADD_TO_ID3_GENRES("Grunge", 6, "\6");
    ADD_TO_ID3_GENRES("Hip-Hop", 7, "\7");
    ADD_TO_ID3_GENRES("Jazz", 4, "\10");
    ADD_TO_ID3_GENRES("Metal", 5, "\11");
    ADD_TO_ID3_GENRES("New Age", 7, "\12");
    ADD_TO_ID3_GENRES("Oldies", 6, "\13");
    ADD_TO_ID3_GENRES("Other", 5, "\14");
    ADD_TO_ID3_GENRES("Pop", 3, "\15");
    ADD_TO_ID3_GENRES("R & B", 5, "\16");
    ADD_TO_ID3_GENRES("Rap", 3, "\17");
    ADD_TO_ID3_GENRES("Reggae", 6, "\20");
    ADD_TO_ID3_GENRES("Rock", 4, "\21");
    ADD_TO_ID3_GENRES("Techno", 6, "\22");
    ADD_TO_ID3_GENRES("Industrial", 10, "\23");
    ADD_TO_ID3_GENRES("Alternative", 11, "\24");
    ADD_TO_ID3_GENRES("Ska", 3, "\25");
    ADD_TO_ID3_GENRES("Death Metal", 11, "\26");
    ADD_TO_ID3_GENRES("Prank", 5, "\27");
    ADD_TO_ID3_GENRES("Soundtrack", 10, "\30");
    ADD_TO_ID3_GENRES("Euro-Techno", 11, "\31");
    ADD_TO_ID3_GENRES("Ambient", 7, "\32");
    ADD_TO_ID3_GENRES("Trip-Hop", 8, "\33");
    ADD_TO_ID3_GENRES("Vocal", 5, "\34");
    ADD_TO_ID3_GENRES("Jazz + Funk", 11, "\35");
    ADD_TO_ID3_GENRES("Fusion", 6, "\36");
    ADD_TO_ID3_GENRES("Trance", 6, "\37");
    ADD_TO_ID3_GENRES("Classical", 9, "\40");
    ADD_TO_ID3_GENRES("Instrumental", 12, "\41");
    ADD_TO_ID3_GENRES("Acid", 4, "\42");
    ADD_TO_ID3_GENRES("House", 5, "\43");
    ADD_TO_ID3_GENRES("Game", 4, "\44");
    ADD_TO_ID3_GENRES("Sound Clip", 10, "\45");
    ADD_TO_ID3_GENRES("Gospel", 6, "\46");
    ADD_TO_ID3_GENRES("Noise", 5, "\47");
    ADD_TO_ID3_GENRES("Alternative Rock", 16, "\50");
    ADD_TO_ID3_GENRES("Bass", 4, "\51");
    ADD_TO_ID3_GENRES("Soul", 4, "\52");
    ADD_TO_ID3_GENRES("Punk", 4, "\53");
    ADD_TO_ID3_GENRES("Space", 5, "\54");
    ADD_TO_ID3_GENRES("Meditative", 10, "\55");
    ADD_TO_ID3_GENRES("Instrumental Pop", 16, "\56");
    ADD_TO_ID3_GENRES("Instrumental Rock", 17, "\57");
    ADD_TO_ID3_GENRES("Ethnic", 6, "\60");
    ADD_TO_ID3_GENRES("Gothic", 6, "\61");
    ADD_TO_ID3_GENRES("Darkwave", 8, "\62");
    ADD_TO_ID3_GENRES("Techno-Industrial", 17, "\63");
    ADD_TO_ID3_GENRES("Electronic", 10, "\64");
    ADD_TO_ID3_GENRES("Pop-Fol", 7, "\65");
    ADD_TO_ID3_GENRES("Eurodance", 9, "\66");
    ADD_TO_ID3_GENRES("Dream", 5, "\67");
    ADD_TO_ID3_GENRES("Southern Rock", 13, "\70");
    ADD_TO_ID3_GENRES("Comedy", 6, "\71");
    ADD_TO_ID3_GENRES("Cult", 4, "\72");
    ADD_TO_ID3_GENRES("Gangsta", 7, "\73");
    ADD_TO_ID3_GENRES("Top 40", 6, "\74");
    ADD_TO_ID3_GENRES("Christian Rap", 13, "\75");
    ADD_TO_ID3_GENRES("Pop/Funk", 8, "\76");
    ADD_TO_ID3_GENRES("Jungle", 6, "\77");
    ADD_TO_ID3_GENRES("Native US", 9, "\100");
    ADD_TO_ID3_GENRES("Cabaret", 7, "\101");
    ADD_TO_ID3_GENRES("New Wave", 8, "\102");
    ADD_TO_ID3_GENRES("Psychadelic", 11, "\103");
    ADD_TO_ID3_GENRES("Rave", 4, "\104");
    ADD_TO_ID3_GENRES("Showtunes", 9, "\105");
    ADD_TO_ID3_GENRES("Trailer", 7, "\106");
    ADD_TO_ID3_GENRES("Lo-Fi", 5, "\107");
    ADD_TO_ID3_GENRES("Tribal", 6, "\110");
    ADD_TO_ID3_GENRES("Acid Punk", 9, "\111");
    ADD_TO_ID3_GENRES("Acid Jazz", 9, "\112");
    ADD_TO_ID3_GENRES("Polka", 5, "\113");
    ADD_TO_ID3_GENRES("Retro", 5, "\114");
    ADD_TO_ID3_GENRES("Musical", 7, "\115");
    ADD_TO_ID3_GENRES("Rock & Roll", 11, "\116");
    ADD_TO_ID3_GENRES("Hard Rock", 9, "\117");
    ADD_TO_ID3_GENRES("Folk", 4, "\120");
    ADD_TO_ID3_GENRES("Folk-Rock", 9, "\121");
    ADD_TO_ID3_GENRES("National Folk", 13, "\122");
    ADD_TO_ID3_GENRES("Swing", 5, "\123");
    ADD_TO_ID3_GENRES("Fast Fusion", 11, "\124");
    ADD_TO_ID3_GENRES("Bebop", 5, "\125");
    ADD_TO_ID3_GENRES("Latin", 5, "\126");
    ADD_TO_ID3_GENRES("Revival", 7, "\127");
    ADD_TO_ID3_GENRES("Celtic", 6, "\130");
    ADD_TO_ID3_GENRES("Bluegrass", 9, "\131");
    ADD_TO_ID3_GENRES("Avantgarde", 10, "\132");
    ADD_TO_ID3_GENRES("Gothic Rock", 11, "\133");
    ADD_TO_ID3_GENRES("Progressive Rock", 16, "\134");
    ADD_TO_ID3_GENRES("Psychedelic Rock", 16, "\135");
    ADD_TO_ID3_GENRES("Symphonic Rock", 14, "\136");
    ADD_TO_ID3_GENRES("Slow Rock", 9, "\137");
    ADD_TO_ID3_GENRES("Big Band", 8, "\140");
    ADD_TO_ID3_GENRES("Chorus", 6, "\141");
    ADD_TO_ID3_GENRES("Easy Listening", 14, "\142");
    ADD_TO_ID3_GENRES("Acoustic", 8, "\143");
    ADD_TO_ID3_GENRES("Humour", 6, "\144");
    ADD_TO_ID3_GENRES("Speech", 6, "\145");
    ADD_TO_ID3_GENRES("Chanson", 7, "\146");
    ADD_TO_ID3_GENRES("Opera", 5, "\147");
    ADD_TO_ID3_GENRES("Chamber Music", 13, "\150");
    ADD_TO_ID3_GENRES("Sonata", 6, "\151");
    ADD_TO_ID3_GENRES("Symphony", 8, "\152");
    ADD_TO_ID3_GENRES("Booty Bass", 10, "\153");
    ADD_TO_ID3_GENRES("Primus", 6, "\154");
    ADD_TO_ID3_GENRES("Porn Groove", 11, "\155");
    ADD_TO_ID3_GENRES("Satire", 6, "\156");
    ADD_TO_ID3_GENRES("Slow Jam", 8, "\157");
    ADD_TO_ID3_GENRES("Club", 4, "\160");
    ADD_TO_ID3_GENRES("Tango", 5, "\161");
    ADD_TO_ID3_GENRES("Samba", 5, "\162");
    ADD_TO_ID3_GENRES("Folklore", 8, "\163");
    ADD_TO_ID3_GENRES("Ballad", 6, "\164");
    ADD_TO_ID3_GENRES("Power Ballad", 12, "\165");
    ADD_TO_ID3_GENRES("Rhytmic Soul", 12, "\166");
    ADD_TO_ID3_GENRES("Freestyle", 9, "\167");
    ADD_TO_ID3_GENRES("Duet", 4, "\170");
    ADD_TO_ID3_GENRES("Punk Rock", 9, "\171");
    ADD_TO_ID3_GENRES("Drum Solo", 9, "\172");
    ADD_TO_ID3_GENRES("Acapella", 8, "\173");
    ADD_TO_ID3_GENRES("Euro-House", 10, "\174");
    ADD_TO_ID3_GENRES("Dance Hall", 10, "\175");
    ADD_TO_ID3_GENRES("Goa", 3, "\176");
    ADD_TO_ID3_GENRES("Drum & Bass", 11, "\177");
    ADD_TO_ID3_GENRES("Club-House", 10, "\200");
    ADD_TO_ID3_GENRES("Hardcore", 8, "\201");
    ADD_TO_ID3_GENRES("Terror", 6, "\202");
    ADD_TO_ID3_GENRES("Indie", 5, "\203");
    ADD_TO_ID3_GENRES("BritPop", 7, "\204");
    ADD_TO_ID3_GENRES("Negerpunk", 9, "\205");
    ADD_TO_ID3_GENRES("Polsk Punk", 10, "\206");
    ADD_TO_ID3_GENRES("Beat", 4, "\207");
    ADD_TO_ID3_GENRES("Christian Gangsta Rap", 21, "\210");
    ADD_TO_ID3_GENRES("Heavy Metal", 11, "\211");
    ADD_TO_ID3_GENRES("Black Metal", 11, "\212");
    ADD_TO_ID3_GENRES("Crossover", 9, "\213");
    ADD_TO_ID3_GENRES("Contemporary Christian", 22, "\214");
    ADD_TO_ID3_GENRES("Christian Rock", 14, "\215");
    ADD_TO_ID3_GENRES("Merengue", 8, "\216");
    ADD_TO_ID3_GENRES("Salsa", 5, "\217");
    ADD_TO_ID3_GENRES("Trash Meta", 10, "\220");
    ADD_TO_ID3_GENRES("Anime", 5, "\221");
    ADD_TO_ID3_GENRES("Jpop", 4, "\222");
    ADD_TO_ID3_GENRES("Synthpop", 8, "\223");
    
    #undef ADD_TO_ID3_GENRES

    return 0;
    
    load_genres_error:
    if(ID3_GENRES != NULL){
        if(ID3_GENRES->close(ID3_GENRES) == -1) {
            tag->error = "db->close";
        }
        ID3_GENRES = NULL;
    }
    return -1;
}