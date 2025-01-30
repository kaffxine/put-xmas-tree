#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#define USAGE_ERR \
    do { \
        fprintf(stderr, "USAGE: %s\n" \
            "  N_GNOMES ORNAMENT_INSTALLATION_TIME_MICROSECONDS\n" \
            "  ORNAMENTS_PER_DELIVERY DELIVERY_INTERVAL_MICROSECONDS N_LEVELS\n" \
            "  GNOME_CAP_0 GNOME_CAP_1 ... GNOME_CAP_N_LEVELS-1\n" \
            "  ORNAMENT_CAP_0 ORNAMENT_CAP_1 ... ORNAMENT_CAP_N_LEVELS-1\n", argv[0]); \
        exit(1); \
    } while (0);


struct level {
    /* the maximum number of gnomes allowed on this level */
    unsigned gnome_cap;

    /* the maximum number of ornaments allowed on this level */
    unsigned ornament_cap;

    /* the current number of gnomes present on this level */
    unsigned n_gnomes_current;

    /* the current number of gnomes waiting to go down from this level */
    unsigned n_gnomes_waiting_down;

    /* the current number of gnomes waiting to go up from this level */
    unsigned n_gnomes_waiting_up;

    /* the current number of ornaments present on this level */
    unsigned n_ornaments_current;

    /* the number of ornaments currently being installed */
    unsigned n_ornaments_pending;

    /* ensure exlusive access to n_gnomes */
    pthread_mutex_t n_gnomes_mutex;

    /* ensure exlusive access to n_ornaments */
    pthread_mutex_t n_ornaments_mutex;

    /* enable waiting for a gnome to free a place from one level up */
    pthread_cond_t waiting_at_upper_cond;

    /* enable waiting for a gnome to free a place from one level down */
    pthread_cond_t waiting_at_lower_cond;
};

struct xmas_tree {
    /* note that levels are indexed from 0 */
    unsigned n_levels;

    struct level *levels;

    unsigned n_gnomes;

    /* this array stores the current level for each gnome */
    /* gnome_positions[i] == -1 <=> gnome number i is not on the tree */
    long *gnome_positions;
};

struct ornament_delivery {
    unsigned n_gnomes_waiting;
    unsigned ornaments_per_delivery;
    useconds_t interval_microseconds;
    unsigned n_ornaments_current;
    pthread_mutex_t n_ornaments_mutex;
    pthread_cond_t n_ornaments_cond;
};

static useconds_t installation_time;
static unsigned long long ornaments_max;
static unsigned long long ornaments_cur = 0;
static pthread_mutex_t ornaments_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct xmas_tree tree;
static struct ornament_delivery delivery;

/* initializes the global tree variable */
/* returns 0 on success, -1 on failure */
int init_xmas_tree(
    unsigned n_gnomes,
    unsigned n_levels,
    unsigned *gnome_cap_list,
    unsigned *ornament_cap_list
) {
    if (n_levels == 0) {
        fprintf(stderr, "init_xmas_tree: "
            "n_levels must be a positive integer");
        return -1;
    }

    if (n_gnomes == 0) {
        fprintf(stderr, "init_xmas_tree: "
            "n_gnomes must be a positive integer");
        return -1;
    }

    for (size_t i = 1; i < n_levels; i++) {
        if (gnome_cap_list[i] >= gnome_cap_list[i - 1]) {
            fprintf(stderr, "init_xmas_tree: "
                "gnomes_cap must be greater on each level than on the level above\n");
            return -1;
        }
    }

    struct level *levels = malloc(n_levels * sizeof(struct level));
    if (levels == (struct level *)0) {
        fprintf(stderr, "init_xmas_tree: "
            "failed to malloc the `levels` list\n");
        return -1;
    }

    long *gnome_positions = malloc(n_gnomes * sizeof(long));
    if (gnome_positions == (long *)0) {
        fprintf(stderr, "init_xmas_tree: "
            "failed to malloc the `gnome_positions` list\n");
        return -1;
    }

    for (size_t i = 0; i < n_levels; i++) {
        levels[i].gnome_cap = gnome_cap_list[i];
        levels[i].ornament_cap = ornament_cap_list[i];
        levels[i].n_gnomes_current = 0;
        levels[i].n_gnomes_waiting_down = 0;
        levels[i].n_gnomes_waiting_up = 0;
        levels[i].n_ornaments_current = 0;
        levels[i].n_ornaments_pending = 0;
        if (pthread_mutex_init(&levels[i].n_gnomes_mutex, NULL) != 0) {
            for (size_t j = 0; j < i; j++) {
                pthread_mutex_destroy(&levels[i].n_gnomes_mutex);
            }
            free(levels);
            fprintf(stderr, "init_xmas_tree: "
                "failed to initialize n_gnomes_mutex for levels[%lu]", i);
            return -1;
        }
        if (pthread_mutex_init(&levels[i].n_ornaments_mutex, NULL) != 0) {
            for (size_t j = 0; j < n_levels; j++) {
                pthread_mutex_destroy(&levels[i].n_gnomes_mutex);
            }
            for (size_t j = 0; j < i; j++) {
                pthread_mutex_destroy(&levels[i].n_ornaments_mutex);
            }
            free(levels);
            fprintf(stderr, "init_xmas_tree: "
                "failed to initialize n_ornaments_mutex for levels[%lu]", i);
            return -1;
        }
        if (pthread_cond_init(&levels[i].waiting_at_lower_cond, NULL) != 0) {
            for (size_t j = 0; j < n_levels; j++) {
                pthread_mutex_destroy(&levels[i].n_gnomes_mutex);
                pthread_mutex_destroy(&levels[i].n_ornaments_mutex);
            }
            for (size_t j = 0; j < i; j++) {
                pthread_cond_destroy(&levels[i].waiting_at_lower_cond);
            }
            free(levels);
            fprintf(stderr, "init_xmas_tree: "
                "failed to initialize waiting_at_lower_cond for levels[%lu]", i);
            return -1;
        }
        if (pthread_cond_init(&levels[i].waiting_at_upper_cond, NULL) != 0) {
            for (size_t j = 0; j < n_levels; j++) {
                pthread_mutex_destroy(&levels[i].n_gnomes_mutex);
                pthread_mutex_destroy(&levels[i].n_ornaments_mutex);
                pthread_cond_destroy(&levels[i].waiting_at_lower_cond);
            }
            for (size_t j = 0; j < i; j++) {
                pthread_cond_destroy(&levels[i].waiting_at_upper_cond);
            }
            free(levels);
            fprintf(stderr, "init_xmas_tree: "
                "failed to initialize waiting_at_lower_cond for levels[%lu]", i);
            return -1;
        }
    }
    
    tree.n_levels = n_levels;
    tree.levels = levels;
    tree.n_gnomes = n_gnomes;
    tree.gnome_positions = gnome_positions;

    return 0;
}

/* deallocates memory associated with the global tree variable */
void kill_xmas_tree() {
    for (size_t i = 0; i < tree.n_levels; i++) {
        pthread_mutex_destroy(&tree.levels[i].n_gnomes_mutex);
        pthread_mutex_destroy(&tree.levels[i].n_ornaments_mutex);
        pthread_cond_destroy(&tree.levels[i].waiting_at_lower_cond);
        pthread_cond_destroy(&tree.levels[i].waiting_at_upper_cond);
    }
    free(tree.levels);
}

int init_ornament_delivery(
    unsigned ornaments_per_delivery,
    useconds_t delivery_interval
) {
    delivery.n_gnomes_waiting = 0;
    delivery.ornaments_per_delivery = ornaments_per_delivery;
    delivery.interval_microseconds = delivery_interval;
    delivery.n_ornaments_current = 0;
    if (pthread_mutex_init(&delivery.n_ornaments_mutex, NULL) != 0) {
        fprintf(stderr,
            "init_ornament_delivery: failed to initialize the mutex\n");
        return -1;
    }
    if (pthread_cond_init(&delivery.n_ornaments_cond, NULL) != 0) {
        pthread_mutex_destroy(&delivery.n_ornaments_mutex);
        fprintf(stderr,
            "init_ornament_delivery: failed to initialize the condition variable\n");
        return -1;
    }

    return 0;
}

void kill_ornament_delivery() {
    pthread_mutex_destroy(&delivery.n_ornaments_mutex);
    pthread_cond_destroy(&delivery.n_ornaments_cond);
}

// call this every time to increment the global counter
void *ornament_hanged(void *arg) {
    pthread_mutex_lock(&ornaments_mutex);
    printf("ornament#%llu hanged\n", ornaments_cur);
    ornaments_cur += 1;
    pthread_mutex_unlock(&ornaments_mutex);

    return NULL;
}

// returns the level the gnome's at after the operation
long go_up_the_tree(long current_level, unsigned gnome_id) {
    // if on the maximum level, stay
    if (current_level == tree.n_levels - 1) {
        printf("gnome#%u stays at level#%lu", gnome_id, current_level);
        return current_level;
    }

    pthread_mutex_lock(&tree.levels[current_level + 1].n_gnomes_mutex);

    printf("------------go_up_the_tree: %u %u\n",
        tree.levels[current_level + 1].n_gnomes_current,
        tree.levels[current_level + 1].gnome_cap);

    // if the destination level has a free space:
    if (
        tree.levels[current_level + 1].n_gnomes_current
        <
        tree.levels[current_level + 1].gnome_cap
    ) {
        // switch the level
        pthread_mutex_lock(&tree.levels[current_level].n_gnomes_mutex);
        tree.levels[current_level].n_gnomes_current -= 1;
        tree.levels[current_level + 1].n_gnomes_current += 1;
        pthread_mutex_unlock(&tree.levels[current_level].n_gnomes_mutex);
        pthread_mutex_unlock(&tree.levels[current_level + 1].n_gnomes_mutex);

        // signal to those waiting for a free space on the current level
        pthread_cond_signal(&tree.levels[current_level].waiting_at_upper_cond);
        pthread_cond_signal(&tree.levels[current_level].waiting_at_lower_cond);

        printf("gnome#%u moves up to level#%lu\n", gnome_id, current_level + 1);
        return current_level + 1;
    }

    char waiting = 0;

    // if the destination level is currently full:
    while (1) {
        printf("gnome#%u waits to move up to level#%lu\n", gnome_id, current_level + 1);

        pthread_cond_wait(
            &tree.levels[current_level + 1].waiting_at_lower_cond,
            &tree.levels[current_level + 1].n_gnomes_mutex
        );
        // there might be a race condition and another gnome might have taken
        // your space, so recheck the condition (if false, wait for another signal)
        if (
            tree.levels[current_level + 1].n_gnomes_current
            < tree.levels[current_level + 1].gnome_cap
        ) {
            pthread_mutex_lock(&tree.levels[current_level].n_gnomes_mutex);
            tree.levels[current_level].n_gnomes_current -= 1;
            tree.levels[current_level + 1].n_gnomes_current += 1;
            tree.levels[current_level].n_gnomes_waiting_up -= 1;
            pthread_mutex_unlock(&tree.levels[current_level].n_gnomes_mutex);
            pthread_mutex_unlock(&tree.levels[current_level + 1].n_gnomes_mutex);

            printf("gnome#%u moves up to level#%lu\n", gnome_id, current_level + 1);
            return current_level + 1;
        }
        printf("gnome#%u retries to move up to level#%lu", gnome_id, current_level + 1);
    }

    // this should never be reached
    printf("gnome#%u had a stroke and fell to the ground floor\n", gnome_id);
    return 0xffffffff;
}

// see go_up_the_tree() for comments
long go_down_the_tree(long current_level, unsigned gnome_id) {
    if (current_level < 0) {
        printf("gnome#%u stays at level#%lu", gnome_id, current_level);
        return -1;
    }

    if (current_level == 0) {
        pthread_mutex_lock(&tree.levels[current_level].n_gnomes_mutex);
        tree.levels[current_level].n_gnomes_current -= 1;
        pthread_mutex_unlock(&tree.levels[current_level].n_gnomes_mutex);
        pthread_cond_signal(&tree.levels[current_level].waiting_at_upper_cond);
        pthread_cond_signal(&tree.levels[current_level].waiting_at_lower_cond);

        printf("gnome#%u moves down to the ground floor\n", gnome_id);
        return -1;
    }

    pthread_mutex_lock(&tree.levels[current_level - 1].n_gnomes_mutex);
    if (
        tree.levels[current_level - 1].n_gnomes_current
        <
        tree.levels[current_level - 1].gnome_cap
    ) {
        pthread_mutex_lock(&tree.levels[current_level].n_gnomes_mutex);
        tree.levels[current_level].n_gnomes_current -= 1;
        tree.levels[current_level - 1].n_gnomes_current += 1;
        pthread_mutex_unlock(&tree.levels[current_level].n_gnomes_mutex);
        pthread_mutex_unlock(&tree.levels[current_level - 1].n_gnomes_mutex);
        pthread_cond_signal(&tree.levels[current_level].waiting_at_lower_cond);
        pthread_cond_signal(&tree.levels[current_level].waiting_at_upper_cond);

        printf("gnome#%u moves down to level#%lu\n", gnome_id, current_level - 1);
        return current_level - 1;
    }

    while (1) {
        printf("gnome#%u waits to move down to level#%lu\n", gnome_id, current_level - 1);

        pthread_cond_wait(
            &tree.levels[current_level - 1].waiting_at_upper_cond,
            &tree.levels[current_level - 1].n_gnomes_mutex
        );
        if (
            tree.levels[current_level - 1].n_gnomes_current
            <
            tree.levels[current_level - 1].gnome_cap
        ) {
            pthread_mutex_lock(&tree.levels[current_level].n_gnomes_mutex);
            tree.levels[current_level].n_gnomes_current -= 1;
            tree.levels[current_level - 1].n_gnomes_current += 1;
            pthread_mutex_unlock(&tree.levels[current_level].n_gnomes_mutex);
            pthread_mutex_unlock(&tree.levels[current_level - 1].n_gnomes_mutex);

            printf("gnome#%u moves down to level#%lu\n", gnome_id, current_level - 1);
            return current_level - 1;
        }
        printf("gnome#%u retries to move up to level#%lu", gnome_id, current_level - 1);
    }

    printf("gnome#%u had a stroke and fell to the ground floor\n", gnome_id);
    return 0xffffffff;
}

// returns 0 on success, -1 on failure
int hang_ornament(unsigned level_id, unsigned gnome_id) {
    pthread_mutex_lock(&tree.levels[level_id].n_ornaments_mutex);
 
    unsigned ornament_id = 
        tree.levels[level_id].n_ornaments_current +
        tree.levels[level_id].n_ornaments_pending;

    // if there still are ornaments to hang...
    if (ornament_id < tree.levels[level_id].ornament_cap) {
        tree.levels[level_id].n_ornaments_pending += 1;
        pthread_mutex_unlock(&tree.levels[level_id].n_ornaments_mutex);

        printf("gnome#%u started hanging an ornament#%u on level#%u\n",
                gnome_id, ornament_id, level_id);
        usleep(installation_time);
        printf("gnome#%u finished hanging an ornament#%u on level#%u\n",
                gnome_id, ornament_id, level_id);

        pthread_mutex_lock(&tree.levels[level_id].n_ornaments_mutex);
        tree.levels[level_id].n_ornaments_pending -= 1;
        tree.levels[level_id].n_ornaments_current += 1;
        pthread_mutex_unlock(&tree.levels[level_id].n_ornaments_mutex);
        
        // increment the global counter
        pthread_t th_inc_orn;
        pthread_create(&th_inc_orn, NULL, ornament_hanged, NULL);
        pthread_detach(th_inc_orn);

        return 0;
    }
    pthread_mutex_unlock(&tree.levels[level_id].n_ornaments_mutex);
    return -1;
}

// this function returning means the gnome has picked up an ornament
void await_ornament(unsigned gnome_id) {
    pthread_mutex_lock(&delivery.n_ornaments_mutex);
    while (delivery.n_ornaments_current == 0) {
        printf("gnome#%u is waiting for an ornament\n", gnome_id);
        pthread_cond_wait(&delivery.n_ornaments_cond, &delivery.n_ornaments_mutex);
    }
    delivery.n_ornaments_current -= 1;
    pthread_mutex_unlock(&delivery.n_ornaments_mutex);
    printf("gnome#%u picked up an ornament\n", gnome_id);
    return;
}

void *gnome(void *arg) {
    unsigned id = *((unsigned *)arg);
    printf("gnome#%u says hi\n", id);

    // 1 if currently carries an ornament, 0 otherwise
    unsigned char has_ornament = 0;

    // current level: -1 if ground
    long level = -1;

    while (1) {
        if (level == -1) {
            // if all the ornaments are hanged, the gnome may rest
            pthread_mutex_lock(&ornaments_mutex);
            if (ornaments_cur == ornaments_max) {
                printf("gnome #%u has finally rested under the christmas tree\n", id);
                pthread_mutex_unlock(&ornaments_mutex);
                break;
            }
            pthread_mutex_unlock(&ornaments_mutex);

            if (!has_ornament) {
                await_ornament(id);
                has_ornament = 1;
            }
            level = go_up_the_tree(level, id);
        } else if (level >= 0) {
            // if no ornament, go down
            if (!has_ornament) {
                level = go_down_the_tree(level, id);
                continue;
            }

            // if successfully hanged an ornament:
            if (hang_ornament(level, id) == 0) {
                has_ornament = 0;
                continue;
            }

            // There is no space for another ornament on the current level.
            // If it's the top level, you can throw the ornament to the floor
            // as you've already checked every level so it's time to give up.
            if (level == tree.n_levels - 1) {
                has_ornament = 0;
                continue;
            }
            
            // Maybe there's still space on the upper levels...
            level = go_up_the_tree(level, id);
        } else {
            break;
        }
    }

    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        USAGE_ERR;
    }

    char *endptr;

    unsigned n_gnomes = strtol(argv[1], &endptr, 10);
    if (endptr == argv[1]) {
        fprintf(stderr, "ERROR: failed to parse N_GNOMES\n");
        USAGE_ERR;
    }

    installation_time = strtol(argv[2], &endptr, 10);
    if (endptr == argv[2]) {
        fprintf(stderr, "ERROR: failed to parse ORNAMENT_INSTALLATION_TIME_MICROSECONDS\n");
        USAGE_ERR;
    }

    unsigned ornaments_per_delivery = strtol(argv[3], &endptr, 10);
    if (endptr == argv[3]) {
        fprintf(stderr, "ERROR: failed to parse ORNAMENTS_PER_DELIVERY\n");
        USAGE_ERR;
    }

    useconds_t delivery_interval_microseconds = strtol(argv[4], &endptr, 10);
    if (endptr == argv[4]) {
        fprintf(stderr, "ERROR: failed to parse DELIVERY_INTERVAL_MICROSECONDS\n");
        USAGE_ERR;
    }

    unsigned n_levels = strtol(argv[5], &endptr, 10);
    if (endptr == argv[5]) {
        fprintf(stderr, "ERROR: failed to parse N_LEVELS\n");
        USAGE_ERR;
    }

    if (argc != 2 * n_levels + 6) {
        USAGE_ERR;
    }

    unsigned *gnome_cap_list;
    gnome_cap_list = malloc(n_levels * sizeof(unsigned));
    if (gnome_cap_list == (unsigned *)0) {
        fprintf(stderr, "ERROR: failed to malloc gnome_cap_list\n");
        exit(1);
    }

    unsigned *ornament_cap_list;
    ornament_cap_list = malloc(n_levels * sizeof(unsigned));
    if (ornament_cap_list == (unsigned *)0) {
        fprintf(stderr, "ERROR: failed to malloc ornament_cap_list\n");
        free(gnome_cap_list);
        exit(1);
    }

    for (size_t i = 0; i < n_levels; i++) {
        size_t arg_i = 6 + i;
        gnome_cap_list[i] = strtol(argv[arg_i], &endptr, 10);
        if (endptr == argv[arg_i]) {
            fprintf(stderr, "ERROR: failed to parse GNOME_CAP_%lu\n", i);
            USAGE_ERR;
        }
    }

    ornaments_max = 0;
    for (size_t i = 0; i < n_levels; i++) {
        size_t arg_i = n_levels + 6 + i;
        ornament_cap_list[i] = strtol(argv[arg_i], &endptr, 10);
        if (endptr == argv[arg_i]) {
            fprintf(stderr, "ERROR: failed to parse ORNAMENT_CAP_%lu\n", i);
            USAGE_ERR;
        }
        ornaments_max += ornament_cap_list[i];
    }

    if (init_ornament_delivery(ornaments_per_delivery, delivery_interval_microseconds) == -1) {
        fprintf(stderr, "ERROR: failed to initialize the delivery static variable\n");
        free(gnome_cap_list);
        free(ornament_cap_list);
        exit(1);
    }

    if (init_xmas_tree(n_gnomes, n_levels, gnome_cap_list, ornament_cap_list) == -1) {
        fprintf(stderr, "ERROR: failed to initialize the tree static variable\n");
        free(gnome_cap_list);
        free(ornament_cap_list);
        kill_ornament_delivery();
        exit(1);
    }

    free(gnome_cap_list);
    free(ornament_cap_list);

    printf("n_gnomes: %u\n", n_gnomes);
    printf("ornaments_max: %llu\n", ornaments_max);
    printf("installation_time %u\n", installation_time);
    printf("ornaments_per_delivery: %u\n", delivery.ornaments_per_delivery);
    printf("delivey_interval: %u\n", delivery.interval_microseconds);
    printf("n_levels: %u\n", tree.n_levels);
    for (size_t i = 0; i < n_levels; i++) {
        printf(
            "  level: %lu\n"
            "    gnome_cap: %u\n"
            "    ornament_cap: %u\n",
            i, tree.levels[i].gnome_cap, tree.levels[i].ornament_cap
        );
    }

    pthread_t *threads = (pthread_t *)malloc(sizeof(pthread_t) * n_gnomes);
    if (threads == (pthread_t *)0) {
        fprintf(stderr, "ERROR: failed to allocate memory for thread handles\n");
        kill_ornament_delivery();
        kill_xmas_tree();
        exit(1);
    }
    
    unsigned *gnome_ids = (unsigned *)malloc(sizeof(unsigned) * n_gnomes);
    if (gnome_ids == (unsigned *)0) {
        fprintf(stderr, "ERROR: failed to allocate memory for gnome_ids\n");
        free(threads);
        kill_ornament_delivery();
        kill_xmas_tree();
        exit(1);
    }

    for (size_t i = 0; i < n_gnomes; i++) {
        gnome_ids[i] = (unsigned)i;
        if (pthread_create(&threads[i], NULL, gnome, &gnome_ids[i]) != 0) {
            fprintf(stderr, "ERROR: failed to allocate memory for thread handles\n");
            free(threads);
            kill_ornament_delivery();
            kill_xmas_tree();
            exit(1);
        }
    }

    // handle ornament delivery in the main thread
    while (1) {
        pthread_mutex_lock(&delivery.n_ornaments_mutex);
        printf("delivery: %u ornaments delivered for a total of %u\n",
            delivery.ornaments_per_delivery,
            delivery.n_ornaments_current + delivery.ornaments_per_delivery);
        delivery.n_ornaments_current += delivery.ornaments_per_delivery;
        pthread_mutex_unlock(&delivery.n_ornaments_mutex);
        pthread_cond_broadcast(&delivery.n_ornaments_cond);
        usleep(delivery.interval_microseconds);
    }
    
    // this should never be reached
    kill_ornament_delivery();
    kill_xmas_tree();
    return 0;
}
