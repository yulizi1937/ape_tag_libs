#include <apetag.h>
#include <err.h>
#include <assert.h>
#include <stdio.h>

int ApeInfo_process(char *);
void ApeTag_print(ApeTag* tag);
void ApeItem_print(ApeItem* item);

/* Process all files on the command line */
int main(int argc, char *argv[]) {
    int ret = 0;
    int i;
    
    if(argc > 1) {
        for(i=1; i<argc; i++) {
            if(ApeInfo_process(argv[i]) != 0) {
                ret = 1;
            }
        }
    }
    else {
        printf("usage: %s file [...]\n", argv[0]);
    }
    
    return ret;
}

/* Print out the fields in the file */
int ApeInfo_process(char* filename) {
    int ret;
    int status;
    FILE* file;
    ApeTag* tag = NULL;
    
    if((file = fopen(filename, "r")) == NULL) {
        warn("%s", filename);
        ret = 1;
        goto apeinfo_process_error;
    }
    
    if((tag = ApeTag_new(file, 0)) == NULL) {
        warn(NULL);
        ret = 1;
        goto apeinfo_process_error;
    }
    
    status = ApeTag_parse(tag);
    if(status == 0) {
        
    } else if(status == -1 || (status == -2 && (ferror(file) != 0))) {
        warn(NULL);
        ret = 1;
        goto apeinfo_process_error;
    } else if(status == -3) {
        warnx("%s", tag->error);
        ret = 1;
        goto apeinfo_process_error;
    }
    
    if(tag->flags & APE_HAS_APE) {
        printf("%s (%i fields):\n", filename, tag->item_count);
        ApeTag_print(tag);
    } else {
        printf("%s: no ape tag\n\n", filename);
    }
    
    ret = 0;
    
    apeinfo_process_error:
    ApeTag_free(tag);
    if(file != NULL) {
        if(fclose(file) != 0) {
            warn(NULL);
        }
    }
    
    return ret;
}

/* Prints all items in the tag, one per line. */
void ApeTag_print(ApeTag* tag) {
    DBT key_dbt, value_dbt;
    
    assert(tag != NULL);
    
    if(tag->fields != NULL) {
        if(tag->fields->seq(tag->fields, &key_dbt, &value_dbt, R_FIRST) == 0) {
            ApeItem_print(*(ApeItem **)(value_dbt.data));
            while(tag->fields->seq(tag->fields, &key_dbt, &value_dbt, R_NEXT) == 0) {
                ApeItem_print(*(ApeItem **)(value_dbt.data));
            }
        }
    }
    printf("\n");
}

/* 
Prints a line with the key and value of the item separated by a colon. Includes
information about the tags flags unless they are the default (read-write UTF8).
*/
void ApeItem_print(ApeItem* item) {
    u_int32_t i;
    char c;
    
    assert(item != NULL);
    assert(item->key != NULL);
    assert(item->value != NULL);

    printf("%s: ", item->key);
    if((item->flags & APE_ITEM_TYPE_FLAGS) == APE_ITEM_BINARY) {
        printf("[BINARY DATA]");
    } else if((item->flags & APE_ITEM_TYPE_FLAGS) == APE_ITEM_RESERVED) {
        printf("[RESERVED]");
    } else {
        if((item->flags & APE_ITEM_TYPE_FLAGS) == APE_ITEM_EXTERNAL) {
            printf("[EXTERNAL LOCATION] ");
        }
        for(i=0; i < item->size; i++) {
            c = *((char *)(item->value)+i);
            if(c == '\0') {
                printf(", ");
            } else if(c < '\40') {
                printf("\\%o", c);
            } else if(c == '\\') {
                printf("\\\\");
            } else {
                printf("%c", c);
            }
        }
    }
    if(item->flags & APE_ITEM_READ_ONLY) {
        printf(" [READ_ONLY]");
    }
    printf("\n");
}
