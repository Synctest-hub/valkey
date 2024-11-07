#include "../hashset.h"
#include "test_help.h"
#include "../mt19937-64.h"
#include "../zmalloc.h"

#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <math.h>


/* From util.c: getRandomBytes to seed hash function. */
void getRandomBytes(unsigned char *p, size_t len);

/* Init hash function salt and seed random generator. */
static void randomSeed(void) {
    unsigned long long seed;
    getRandomBytes((void *)&seed, sizeof(seed));
    init_genrand64(seed);
    srandom((unsigned)seed);
}

/* An element holding a string key and a string value in one allocation. */
typedef struct {
    unsigned int keysize; /* Sizes, including null-terminator */
    unsigned int valsize;
    char data[]; /* key and value */
} keyval;

static keyval *create_keyval(const char *key, const char *val) {
    size_t keysize = strlen(key) + 1;
    size_t valsize = strlen(val) + 1;
    keyval *e = malloc(sizeof(keyval) + keysize + valsize);
    e->keysize = keysize;
    e->valsize = valsize;
    memcpy(e->data, key, keysize);
    memcpy(e->data + keysize, val, valsize);
    return e;
}

static const void *getkey(const void *element) {
    const keyval *e = element;
    return e->data;
}

static const void *getval(const void *element) {
    const keyval *e = element;
    return e->data + e->keysize;
}

static uint64_t hashfunc(const void *key) {
    return hashsetGenHashFunction(key, strlen(key));
}

static int keycmp(hashset *ht, const void *key1, const void *key2) {
    (void)ht;
    return strcmp(key1, key2);
}

static void freekeyval(hashset *ht, void *keyval) {
    (void)ht;
    free(keyval);
}

/* Hashset type used for some of the tests. */
static hashsetType keyval_type = {
    .elementGetKey = getkey,
    .hashFunction = hashfunc,
    .keyCompare = keycmp,
    .elementDestructor = freekeyval,
};

/* Callback for testing hashsetEmpty(). */
static long empty_callback_call_counter;
void emptyCallback(hashset *s) {
    UNUSED(s);
    empty_callback_call_counter++;
}

/* Prototypes for debugging */
void hashsetDump(hashset *s);
void hashsetHistogram(hashset *s);
int hashsetLongestBucketChain(hashset *s);
size_t nextCursor(size_t v, size_t mask);

int test_cursor(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    TEST_ASSERT(nextCursor(0x0000, 0xffff) == 0x8000);
    TEST_ASSERT(nextCursor(0x8000, 0xffff) == 0x4000);
    TEST_ASSERT(nextCursor(0x4001, 0xffff) == 0xc001);
    TEST_ASSERT(nextCursor(0xffff, 0xffff) == 0x0000);
    return 0;
}

int test_set_hash_function_seed(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    randomSeed();
    return 0;
}

static void add_find_delete_test_helper(int flags) {
    int count = (flags & UNIT_TEST_ACCURATE) ? 1000000 : 200;
    hashset *s = hashsetCreate(&keyval_type);
    int j;

    /* Add */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        keyval *e = create_keyval(key, val);
        /* printf("Adding %s => %s\n", key, val); */
        assert(hashsetAdd(s, e));
    }

    if (count < 1000) {
        hashsetHistogram(s);
    }

    /* Find */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        keyval *e;
        assert(hashsetFind(s, key, (void **)&e));
        assert(!strcmp(val, getval(e)));
    }

    /* Delete half of them */
    for (j = 0; j < count / 2; j++) {
        char key[32];
        snprintf(key, sizeof(key), "%d", j);
        if (j % 3 == 0) {
            /* Test hashsetPop */
            char val[32];
            snprintf(val, sizeof(val), "%d", count - j + 42);
            keyval *e;
            assert(hashsetPop(s, key, (void **)&e));
            assert(!strcmp(val, getval(e)));
            free(e);
        } else {
            assert(hashsetDelete(s, key));
        }
    }

    /* Empty, i.e. delete remaining elements, with progress callback. */
    empty_callback_call_counter = 0;
    hashsetEmpty(s, emptyCallback);
    assert(empty_callback_call_counter > 0);

    /* Release memory */
    hashsetRelease(s);
}

int test_add_find_delete(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    add_find_delete_test_helper(flags);
    TEST_ASSERT(zmalloc_used_memory() == 0);
    return 0;
}

int test_add_find_delete_avoid_resize(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    hashsetSetResizePolicy(HASHSET_RESIZE_AVOID);
    add_find_delete_test_helper(flags);
    hashsetSetResizePolicy(HASHSET_RESIZE_ALLOW);
    TEST_ASSERT(zmalloc_used_memory() == 0);
    return 0;
}

int test_instant_rehashing(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    long count = 200;

    /* A set of longs, i.e. pointer-sized values. */
    hashsetType type = {.instant_rehashing = 1};
    hashset *s = hashsetCreate(&type);
    long j;

    /* Populate and check that rehashing is never ongoing. */
    for (j = 0; j < count; j++) {
        assert(hashsetAdd(s, (void *)j));
        assert(!hashsetIsRehashing(s));
    }

    /* Delete and check that rehashing is never ongoing. */
    for (j = 0; j < count; j++) {
        assert(hashsetDelete(s, (void *)j));
        assert(!hashsetIsRehashing(s));
    }

    hashsetRelease(s);
    return 0;
}


int test_bucket_chain_length(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    unsigned long count = 1000000;

    /* A set of longs, i.e. pointer-sized integer values. */
    hashsetType type = {0};
    hashset *s = hashsetCreate(&type);
    unsigned long j;
    for (j = 0; j < count; j++) {
        assert(hashsetAdd(s, (void *)j));
    }
    /* If it's rehashing, add a few more until rehashing is complete. */
    while (hashsetIsRehashing(s)) {
        j++;
        assert(hashsetAdd(s, (void *)j));
    }
    TEST_ASSERT(j < count * 2);
    int max_chainlen_not_rehashing = hashsetLongestBucketChain(s);
    TEST_ASSERT(max_chainlen_not_rehashing < 10);

    /* Add more until rehashing starts again. */
    while (!hashsetIsRehashing(s)) {
        j++;
        assert(hashsetAdd(s, (void *)j));
    }
    TEST_ASSERT(j < count * 2);
    int max_chainlen_rehashing = hashsetLongestBucketChain(s);
    TEST_ASSERT(max_chainlen_rehashing < 10);

    hashsetRelease(s);
    return 0;
}

int test_two_phase_insert_and_pop(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    int count = (flags & UNIT_TEST_ACCURATE) ? 1000000 : 200;
    hashset *s = hashsetCreate(&keyval_type);
    int j;

    /* hashsetFindPositionForInsert + hashsetInsertAtPosition */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        hashsetPosition position;
        int ret = hashsetFindPositionForInsert(s, key, &position, NULL);
        assert(ret == 1);
        keyval *e = create_keyval(key, val);
        hashsetInsertAtPosition(s, e, &position);
    }

    if (count < 1000) {
        hashsetHistogram(s);
    }

    /* Check that all elements were inserted. */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        keyval *e;
        assert(hashsetFind(s, key, (void **)&e));
        assert(!strcmp(val, getval(e)));
    }

    /* Test two-phase pop. */
    for (j = 0; j < count; j++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "%d", j);
        snprintf(val, sizeof(val), "%d", count - j + 42);
        hashsetPosition position;
        size_t size_before_find = hashsetSize(s);
        void **ref = hashsetTwoPhasePopFindRef(s, key, &position);
        assert(ref != NULL);
        keyval *e = *ref;
        assert(!strcmp(val, getval(e)));
        assert(hashsetSize(s) == size_before_find);
        hashsetTwoPhasePopDelete(s, &position);
        assert(hashsetSize(s) == size_before_find - 1);
    }
    assert(hashsetSize(s) == 0);

    hashsetRelease(s);
    return 0;
}

typedef struct {
    long count;
    uint8_t element_seen[];
} scandata;

void scanfn(void *privdata, void *element) {
    scandata *data = (scandata *)privdata;
    unsigned long j = (unsigned long)element;
    data->element_seen[j]++;
    data->count++;
}

int test_scan(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    long num_elements = (flags & UNIT_TEST_LARGE_MEMORY) ? 1000000 : 200000;
    int num_rounds = (flags & UNIT_TEST_ACCURATE) ? 20 : 5;

    /* A set of longs, i.e. pointer-sized values. */
    hashsetType type = {0};
    long j;

    for (int round = 0; round < num_rounds; round++) {
        /* First round count = num_elements, then some more. */
        long count = num_elements * (1 + 2 * (double)round / num_rounds);

        /* Seed, to make sure each round is different. */
        randomSeed();

        /* Populate */
        hashset *s = hashsetCreate(&type);
        for (j = 0; j < count; j++) {
            assert(hashsetAdd(s, (void *)j));
        }

        /* Scan */
        scandata *data = calloc(1, sizeof(scandata) + count);
        long max_elements_per_cycle = 0;
        unsigned num_cycles = 0;
        long scanned_count = 0;
        size_t cursor = 0;
        do {
            data->count = 0;
            cursor = hashsetScan(s, cursor, scanfn, data);
            if (data->count > max_elements_per_cycle) {
                max_elements_per_cycle = data->count;
            }
            scanned_count += data->count;
            data->count = 0;
            num_cycles++;
        } while (cursor != 0);

        /* Verify every element was returned at least once, but no more than
         * twice. Elements can be returned twice due to probing chains wrapping
         * around scan cursor zero. */
        TEST_ASSERT(scanned_count >= count);
        TEST_ASSERT(scanned_count < count * 2);
        for (j = 0; j < count; j++) {
            assert(data->element_seen[j] >= 1);
            assert(data->element_seen[j] <= 2);
        }

        /* Verify some stuff, but just print it for now. */
        printf("Scanned: %lu; ", count);
        printf("duplicates emitted: %lu; ", scanned_count - count);
        printf("max emitted per call: %ld; ", max_elements_per_cycle);
        printf("avg emitted per call: %.2lf\n", (double)count / num_cycles);

        /* Cleanup */
        hashsetRelease(s);
        free(data);
    }
    return 0;
}

typedef struct {
    uint64_t value;
    uint64_t hash;
} mock_hash_element;

static mock_hash_element *mock_hash_element_create(uint64_t value, uint64_t hash) {
    mock_hash_element *element = malloc(sizeof(mock_hash_element));
    element->value = value;
    element->hash = hash;
    return element;
}

static uint64_t mock_hash_element_get_hash(const void *element) {
    if (element == NULL) return 0UL;
    mock_hash_element *mock = (mock_hash_element *)element;
    return (mock->hash != 0) ? mock->hash : mock->value;
}

int test_iterator(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    size_t count = 2000000;
    uint8_t element_array[count];
    memset(element_array, 0, sizeof element_array);

    /* A set of uint8_t pointers */
    hashsetType type = {0};
    hashset *s = hashsetCreate(&type);

    /* Populate */
    for (size_t j = 0; j < count; j++) {
        assert(hashsetAdd(s, element_array + j));
    }

    /* Iterate */
    size_t num_returned = 0;
    hashsetIterator iter;
    hashsetInitIterator(&iter, s);
    uint8_t *element;
    while (hashsetNext(&iter, (void **)&element)) {
        num_returned++;
        assert(element >= element_array && element < element_array + count);
        /* increment element at this position as a counter */
        (*element)++;
    }
    hashsetResetIterator(&iter);

    /* Check that all elements were returned exactly once. */
    TEST_ASSERT(num_returned == count);
    for (size_t j = 0; j < count; j++) {
        if (element_array[j] != 1) {
            printf("Element %zu returned %d times\n", j, element_array[j]);
            return 0;
        }
    }

    hashsetRelease(s);
    return 0;
}

int test_safe_iterator(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    size_t count = 1000;
    uint8_t element_counts[count * 2];
    memset(element_counts, 0, sizeof element_counts);

    /* A set of longs, i.e. pointer-sized values. */
    hashsetType type = {0};
    hashset *s = hashsetCreate(&type);

    /* Populate */
    for (size_t j = 0; j < count; j++) {
        assert(hashsetAdd(s, element_counts + j));
    }

    /* Iterate */
    size_t num_returned = 0;
    hashsetIterator iter;
    hashsetInitSafeIterator(&iter, s);
    uint8_t *element;
    while (hashsetNext(&iter, (void **)&element)) {
        size_t index = element - element_counts;
        num_returned++;
        assert(element >= element_counts && element < element_counts + count * 2);
        /* increment element at this position as a counter */
        (*element)++;
        if (index % 4 == 0) {
            assert(hashsetDelete(s, element));
        }
        /* Add new item each time we see one of the original items */
        if (index < count) {
            assert(hashsetAdd(s, element + count));
        }
    }
    hashsetResetIterator(&iter);

    /* Check that all elements present during the whole iteration were returned
     * exactly once. (Some are deleted after being returned.) */
    TEST_ASSERT(num_returned >= count);
    for (size_t j = 0; j < count; j++) {
        if (element_counts[j] != 1) {
            printf("Element %zu returned %d times\n", j, element_counts[j]);
            return 0;
        }
    }
    /* Check that elements inserted during the iteration were returned at most
     * once. */
    unsigned long num_optional_returned = 0;
    for (size_t j = count; j < count * 2; j++) {
        assert(element_counts[j] <= 1);
        num_optional_returned += element_counts[j];
    }
    printf("Safe iterator returned %lu of the %zu elements inserted while iterating.\n", num_optional_returned, count);

    hashsetRelease(s);
    return 0;
}

int test_random_element(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    randomSeed();

    size_t count = (flags & UNIT_TEST_LARGE_MEMORY) ? 7000 : 400;
    long num_rounds = (flags & UNIT_TEST_ACCURATE) ? 1000000 : 10000;

    /* A set of ints */
    hashsetType type = {0};
    hashset *s = hashsetCreate(&type);

    /* Populate */
    unsigned times_picked[count];
    memset(times_picked, 0, sizeof(times_picked));
    for (size_t j = 0; j < count; j++) {
        assert(hashsetAdd(s, times_picked + j));
    }

    /* Pick elements, and count how many times each element is picked. */
    for (long i = 0; i < num_rounds; i++) {
        /* Using void* variable to avoid a cast that violates strict aliasing */
        unsigned *element;
        assert(hashsetFairRandomElement(s, (void **)&element));

        assert(element >= times_picked && element < times_picked + count);
        /* increment element at this position as a counter */
        (*element)++;
    }
    hashsetRelease(s);

    /* Fairness measurement
     * --------------------
     *
     * Selecting a single random element: For any element in the hash table, let
     * X=1 if the we selected the element (success) and X=0 otherwise. With m
     * elements, our element is sepected with probability p = 1/m, the expected
     * value is E(X) = 1/m, E(X^2) = 1/m and the variance:
     *
     *     Var(X) = E(X^2) - (E(X))^2 = 1/m - 1/(m^2) = (1/m) * (1 - 1/m).
     *
     * Repeating the selection of a random element: Let's repeat the experiment
     * n times and let Y be the number of times our element was selected. This
     * is a binomial distribution.
     *
     *     Y = X_1 + X_2 + ... + X_n
     *     E(Y) = n/m
     *
     * The variance of a sum of independent random variables is the sum of the
     * variances, so Y has variance np(1−p).
     *
     *     Var(Y) = npq = np(1 - p) = (n/m) * (1 - 1/m) = n * (m - 1) / (m * m)
     */
    double m = (double)count, n = (double)num_rounds;
    double expected = n / m;                 /* E(Y) */
    double variance = n * (m - 1) / (m * m); /* Var(Y) */
    double std_dev = sqrt(variance);

    /* With large n, the distribution approaches a normal distribution and we
     * can use p68 = within 1 std dev, p95 = within 2 std dev, p99.7 = within 3
     * std dev. */
    long p68 = 0, p95 = 0, p99 = 0, p4dev = 0, p5dev = 0;
    for (size_t j = 0; j < count; j++) {
        double dev = expected - times_picked[j];
        p68 += (dev >= -std_dev && dev <= std_dev);
        p95 += (dev >= -std_dev * 2 && dev <= std_dev * 2);
        p99 += (dev >= -std_dev * 3 && dev <= std_dev * 3);
        p4dev += (dev >= -std_dev * 4 && dev <= std_dev * 4);
        p5dev += (dev >= -std_dev * 5 && dev <= std_dev * 5);
    }
    printf("Random element fairness test\n");
    printf("  Pick one of %zu elements, %ld times.\n", count, num_rounds);
    printf("  Expecting each element to be picked %.2lf times, std dev %.3lf.\n", expected, std_dev);
    printf("  Within 1 std dev (p68) = %.2lf%%\n", 100 * p68 / m);
    printf("  Within 2 std dev (p95) = %.2lf%%\n", 100 * p95 / m);
    printf("  Within 3 std dev (p99) = %.2lf%%\n", 100 * p99 / m);
    printf("  Within 4 std dev       = %.2lf%%\n", 100 * p4dev / m);
    printf("  Within 5 std dev       = %.2lf%%\n", 100 * p5dev / m);

    /* Conclusion? The number of trials (n) relative to the probabilities (p and
     * 1 − p) must be sufficiently large (n * p ≥ 5 and n * (1 − p) ≥ 5) to
     * approximate a binomial distribution with a normal distribution. */
    if (n / m >= 5 && n * (1 - 1 / m) >= 5) {
        TEST_ASSERT_MESSAGE("Too unfair randomness", 100 * p99 / m >= 60.0);
    } else {
        printf("To uncertain numbers to draw any conclusions about fairness.\n");
    }
    return 0;
}

int test_random_element_with_long_chain(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* We use an estimator of true probability.
     * We determine how many samples to take based on how precise of a
     * measurement we want to take, and how certain we want to be that the
     * measurement is correct.
     * https://en.wikipedia.org/wiki/Checking_whether_a_coin_is_fair#Estimator_of_true_probability
     */

    /* In a thousand runs the worst deviation seen was 0.018 +/- 0.01.
     * This means the true deviation was at least 0.008 or 0.8% */
    const double acceptable_probability_deviation = 0.015;

    const size_t num_chained_elements = 64;
    const size_t num_random_elements = 448;
    const double p_fair = (double)num_chained_elements / (num_chained_elements + num_random_elements);

    /* Precision of our measurement */
    const double precision = (flags & UNIT_TEST_ACCURATE) ? 0.001 : 0.01;

    /* This is confidence level for our measurement as the Z value of a normal
     * distribution. 5 sigma corresponds to 0.00002% probability that our
     * measurement is farther than 'precision' from the truth. This value is
     * used in particle physics. */
    const double z = 5;

    const double n = p_fair * (1 - p_fair) * z * z / (precision * precision);
    const size_t num_samples = (size_t)n + 1;

    hashsetType type = {
        .hashFunction = mock_hash_element_get_hash,
        .elementDestructor = freekeyval,
    };

    hashset *s = hashsetCreate(&type);
    hashsetExpand(s, num_random_elements + num_chained_elements);
    uint64_t chain_hash = (uint64_t)genrand64_int64();
    if (chain_hash == 0) chain_hash++;

    /* add random elements */
    for (size_t i = 0; i < num_random_elements; i++) {
        uint64_t random_hash = (uint64_t)genrand64_int64();
        if (random_hash == chain_hash) random_hash++;
        hashsetAdd(s, mock_hash_element_create(random_hash, 0));
    }

    /* create long chain */
    for (size_t i = 0; i < num_chained_elements; i++) {
        hashsetAdd(s, mock_hash_element_create(i, chain_hash));
    }

    assert(!hashsetIsRehashing(s));

    printf("Created a table with a long bucket chain.\n");
    hashsetHistogram(s);

    printf("Taking %zu random samples\n", num_samples);
    size_t count_chain_element_picked = 0;
    for (size_t i = 0; i < num_samples; i++) {
        mock_hash_element *element;
        assert(hashsetFairRandomElement(s, (void **)&element));
        if (element->hash == chain_hash) {
            count_chain_element_picked++;
        }
    }
    const double measured_probability = (double)count_chain_element_picked / num_samples;
    const double deviation = fabs(measured_probability - p_fair);
    printf("Measured probability: %.1f%%\n", measured_probability * 100);
    printf("Expected probability: %.1f%%\n", p_fair * 100);
    printf("Measured probability deviated %1.1f%% +/- %1.1f%% from expected probability\n",
           deviation * 100, precision * 100);
    TEST_ASSERT(deviation <= precision + acceptable_probability_deviation);

    hashsetRelease(s);
    return 0;
}

int test_all_memory_freed(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    TEST_ASSERT(zmalloc_used_memory() == 0);
    return 0;
}
