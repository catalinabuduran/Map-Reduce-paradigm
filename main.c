#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>

#define MAX_FILENAME_LEN 256
#define MAX_WORD_LEN 256
#define MAX_FILES 10000
#define ALPHABET_SIZE 26

typedef struct {
    char *word;
    int *file_ids;
    int file_count;
    int file_capacity;
} WordEntry;

typedef struct {
    WordEntry **entries;
    int entry_count;
    int entry_capacity;
} WordList;

typedef struct {
    char **file_list;
    int *current_index;
    int file_count;
    pthread_mutex_t *queue_mutex;
    pthread_mutex_t *mutexes;
    WordList **word_lists;
} MapperArgs;

typedef struct {
    pthread_mutex_t *mutexes;
    WordList **word_lists;
    int start_letter;
    int end_letter;
    char *output_path;
} ReducerArgs;
typedef struct {
    // 1 pentru Mapper, 0 pentru Reducer
    int is_mapper;
    int start_letter;
    int end_letter;
    pthread_barrier_t *barrier;
    MapperArgs mapper_args;
    ReducerArgs reducer_args;
} ThreadArgs;

void normalize_word(char *word) {
    int i, j = 0;
    for (i = 0; word[i]; i++) {
        if (isalpha(word[i])) {
            word[j++] = tolower(word[i]);
        }
    }
    word[j] = '\0';
}

void add_word(WordList *list, const char *word, int file_id) {
    if (list->entries == NULL) {
        list->entry_capacity = 64;
        list->entries = malloc(list->entry_capacity * sizeof(WordEntry *));
        if (list->entries == NULL) {
            perror("Memory allocation failed for WordList");
            exit(1);
        }
    }

    for (int i = 0; i < list->entry_count; i++) {
        if (strcmp(list->entries[i]->word, word) == 0) {
            for (int j = 0; j < list->entries[i]->file_count; j++) {
                if (list->entries[i]->file_ids[j] == file_id) {
                    return;
                }
            }
            if (list->entries[i]->file_count == list->entries[i]->file_capacity) {
                list->entries[i]->file_capacity *= 2;
                list->entries[i]->file_ids = realloc(list->entries[i]->file_ids, list->entries[i]->file_capacity * sizeof(int));
                if (list->entries[i]->file_ids == NULL) {
                    perror("reallocation failed");
                    exit(1);
                }
            }
            list->entries[i]->file_ids[list->entries[i]->file_count++] = file_id;
            return;
        }
    }

    if (list->entry_count == list->entry_capacity) {
        list->entry_capacity *= 2;
        list->entries = realloc(list->entries, list->entry_capacity * sizeof(WordEntry *));
        if (list->entries == NULL) {
            perror("reallocation failed");
            exit(1);
        }
    }

    WordEntry *entry = malloc(sizeof(WordEntry));
    if (entry == NULL) {
        perror("allocation failed");
        exit(1);
    }
    entry->word = strdup(word);
    entry->file_capacity = 16;
    entry->file_ids = malloc(entry->file_capacity * sizeof(int));
    if (entry->file_ids == NULL) {
        perror("allocation failed");
        exit(1);
    }
    entry->file_ids[0] = file_id;
    entry->file_count = 1;
    list->entries[list->entry_count++] = entry;
}

void list_files_from_file(const char *input_path, char **file_list, int *file_count) {
    struct stat path_stat;
    stat(input_path, &path_stat);

    if (S_ISDIR(path_stat.st_mode)) {
        DIR *d = opendir(input_path);
        if (!d) {
            perror("Error opening directory");
            exit(1);
        }

        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            if (*file_count >= MAX_FILES) {
                fprintf(stderr, "Too many files\n");
                closedir(d);
                exit(1);
            }

            char full_path[MAX_FILENAME_LEN];
            int written = snprintf(full_path, MAX_FILENAME_LEN, "%s/%s", input_path, entry->d_name);
            if (written < 0 || written >= MAX_FILENAME_LEN) {
                closedir(d);
                exit(1);
            }
            snprintf(file_list[*file_count], MAX_FILENAME_LEN, "%s", full_path);
            (*file_count)++;
        }
        closedir(d);
    } else if (S_ISREG(path_stat.st_mode)) {
        FILE *fp = fopen(input_path, "r");
        if (!fp) {
            perror("Error opening file");
            exit(1);
        }

        char line[MAX_FILENAME_LEN];
        int line_number = 0;
        while (fgets(line, sizeof(line), fp)) {
            line_number++;
            if (line_number == 1) continue;

            line[strcspn(line, "\n")] = '\0';
            snprintf(file_list[*file_count], MAX_FILENAME_LEN, "%s", line);
            (*file_count)++;
        }
        fclose(fp);
    } else {
        fprintf(stderr, "Invalid input path: %s\n", input_path);
        exit(1);
    }
}

void *mapper(void *args) {
    MapperArgs *mapper_args = (MapperArgs *)args;
    char buffer[MAX_WORD_LEN];

    while (1) {
        int file_index;

        pthread_mutex_lock(mapper_args->queue_mutex);
        if (*(mapper_args->current_index) >= mapper_args->file_count) {
            pthread_mutex_unlock(mapper_args->queue_mutex);
            break;
        }
        file_index = (*(mapper_args->current_index))++;
        pthread_mutex_unlock(mapper_args->queue_mutex);

        FILE *fp = fopen(mapper_args->file_list[file_index], "r");
        if (!fp) {
            fprintf(stderr, "Error opening file: %s\n", mapper_args->file_list[file_index]);
            continue;
        }

        while (fscanf(fp, "%255s", buffer) != EOF) {
            normalize_word(buffer);
            if (strlen(buffer) > 0) {
                char first_letter = buffer[0];
                if (first_letter >= 'a' && first_letter <= 'z') {
                    int index = first_letter - 'a';
                    pthread_mutex_lock(&mapper_args->mutexes[index]);
                    add_word(mapper_args->word_lists[index], buffer, file_index + 1);
                    pthread_mutex_unlock(&mapper_args->mutexes[index]);
                }
            }
        }
        fclose(fp);
    }
    return NULL;
}

int compare_word_entries(const void *a, const void *b) {
    WordEntry *entryA = *(WordEntry **)a;
    WordEntry *entryB = *(WordEntry **)b;

    if (entryB->file_count != entryA->file_count) {
        return entryB->file_count - entryA->file_count;
    }
    return strcmp(entryA->word, entryB->word);
}

int compare_ints(const void *a, const void *b) {
    return (*(int *)a - *(int *)b);
}

void *reducer(void *args) {
    ReducerArgs *reducer_args = (ReducerArgs *)args;

    for (int letter = reducer_args->start_letter; letter <= reducer_args->end_letter; letter++) {
        pthread_mutex_lock(&reducer_args->mutexes[letter]);

        char file_name[512];
        snprintf(file_name, sizeof(file_name), "%s/%c.txt", reducer_args->output_path, 'a' + letter);
        FILE *output_file = fopen(file_name, "w");
        if (!output_file) {
            fprintf(stderr, "Error opening output file: %s\n", file_name);
            pthread_mutex_unlock(&reducer_args->mutexes[letter]);
            continue;
        }

        WordList *word_list = reducer_args->word_lists[letter];
        if (word_list->entry_count > 0) {
            qsort(word_list->entries, word_list->entry_count, sizeof(WordEntry *), compare_word_entries);

            for (int i = 0; i < word_list->entry_count; i++) {
                WordEntry *entry = word_list->entries[i];
                qsort(entry->file_ids, entry->file_count, sizeof(int), compare_ints);
                fprintf(output_file, "%s:[", entry->word);
                for (int j = 0; j < entry->file_count; j++) {
                    fprintf(output_file, "%d%s", entry->file_ids[j], (j < entry->file_count - 1) ? " " : "");
                }
                fprintf(output_file, "]\n");
            }
        }

        fclose(output_file);
        pthread_mutex_unlock(&reducer_args->mutexes[letter]);
    }
    return NULL;
}

void *combined_thread_function(void *args) {
    ThreadArgs *thread_args = (ThreadArgs *)args;

    if (thread_args->is_mapper) {
        mapper(&thread_args->mapper_args);
    }

    pthread_barrier_wait(thread_args->barrier);

    if (!thread_args->is_mapper) {
        reducer(&thread_args->reducer_args);
    }

    return NULL;
}

int main(int argc, char **argv) {
    int num_mappers = atoi(argv[1]);
    int num_reducers = atoi(argv[2]);
    char *input_path = argv[3];
    char *output_path = argv[4];

    if (argc == 5) {
        output_path = argv[4];
    } else {
        output_path = "./";
    }

    struct stat st_out = {0};
    if (stat(output_path, &st_out) == -1) {
        mkdir(output_path, 0700);
    }

    char **file_list = malloc(MAX_FILES * sizeof(char *));
    for (int i = 0; i < MAX_FILES; i++) {
        file_list[i] = malloc(MAX_FILENAME_LEN);
    }
    int file_count = 0;

    list_files_from_file(input_path, file_list, &file_count);

    pthread_mutex_t mutexes[ALPHABET_SIZE];
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        pthread_mutex_init(&mutexes[i], NULL);
    }

    WordList *word_lists[ALPHABET_SIZE];
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        word_lists[i] = malloc(sizeof(WordList));
        word_lists[i]->entries = NULL;
        word_lists[i]->entry_count = 0;
        word_lists[i]->entry_capacity = 64;
    }

    int current_index = 0;
    pthread_mutex_t queue_mutex;
    pthread_mutex_init(&queue_mutex, NULL);

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, num_mappers + num_reducers);

    int total_threads = num_mappers + num_reducers;
    pthread_t *threads = malloc(total_threads * sizeof(pthread_t));
    ThreadArgs *thread_args = malloc(total_threads * sizeof(ThreadArgs));

    for (int i = 0; i < total_threads; i++) {
        if (i < num_mappers) {
            thread_args[i].is_mapper = 1;
            thread_args[i].mapper_args.file_list = file_list;
            thread_args[i].mapper_args.current_index = &current_index;
            thread_args[i].mapper_args.file_count = file_count;
            thread_args[i].mapper_args.queue_mutex = &queue_mutex;
            thread_args[i].mapper_args.mutexes = mutexes;
            thread_args[i].mapper_args.word_lists = word_lists;
        } else {
            int reducer_index = i - num_mappers;
            thread_args[i].is_mapper = 0;
            thread_args[i].reducer_args.start_letter = reducer_index * (ALPHABET_SIZE / num_reducers);
            thread_args[i].reducer_args.end_letter = (reducer_index + 1) * (ALPHABET_SIZE / num_reducers) - 1;
            if (reducer_index == num_reducers - 1) {
                thread_args[i].reducer_args.end_letter = ALPHABET_SIZE - 1;
            }
            thread_args[i].reducer_args.mutexes = mutexes;
            thread_args[i].reducer_args.word_lists = word_lists;
            thread_args[i].reducer_args.output_path = output_path;
        }
        thread_args[i].barrier = &barrier;
    }

    for (int i = 0; i < total_threads; i++) {
        pthread_create(&threads[i], NULL, combined_thread_function, &thread_args[i]);
    }

    for (int i = 0; i < total_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_barrier_destroy(&barrier);
    pthread_mutex_destroy(&queue_mutex);
    for (int i = 0; i < ALPHABET_SIZE; i++) {
        pthread_mutex_destroy(&mutexes[i]);
        for (int j = 0; j < word_lists[i]->entry_count; j++) {
            WordEntry *entry = word_lists[i]->entries[j];
            free(entry->word);
            free(entry->file_ids);
            free(entry);
        }
        free(word_lists[i]->entries);
        free(word_lists[i]);
    }

    for (int i = 0; i < MAX_FILES; i++) {
        free(file_list[i]);
    }
    free(file_list);

    free(threads);
    free(thread_args);

    return 0;
}