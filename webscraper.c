/**
 * x.com/7etsuo - 2025
 *
 * A web scraper that downloads images from a given URL.
 * The program uses libcurl to download HTML content from a URL,
 * libxml2 to parse the HTML content and extract image URLs, and libcurl to
 * download the images. The program uses a hash table to store unique image URLs
 * and multiple threads to download images concurrently. The downloaded images
 * are saved in a directory named "downloaded_images".
 *
 * gcc -o webscraper webscraper.c $(curl-config --cflags --libs) $(xml2-config --cflags --libs) -pthread
 * ./webscraper https://www.example.com
 */

#include <curl/curl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <libxml/uri.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#define INITIAL_HASH_SIZE 101
#define MAX_LOAD_FACTOR 0.75
#define NUM_THREADS 4
#define OUTPUT_DIR "downloaded_images"
#define DOWNLOAD_DELAY_MS 500
#define REQUEST_DELAY_MS 1000

typedef struct hash_entry
{
  char *url;
  struct hash_entry *next;
} hash_entry_t;

typedef struct
{
  char **urls;
  int count;
  int capacity;
  hash_entry_t **hash_table;
  int hash_size;
  int hash_count;
  char *base_domain;
} url_collection_t;

struct MemoryChunk
{
  char *data;
  size_t size;
};

struct FileWriterData
{
  FILE *fp;
};

struct ThreadData
{
  url_collection_t *collection;
  int thread_id;
};

pthread_mutex_t url_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t domain_mutex = PTHREAD_MUTEX_INITIALIZER;
int url_index = 0;

// clang-format off
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp);
static size_t WriteFileCallback(void *contents, size_t size, size_t nmemb, void *userp);
char *download_html(const char *url);
int is_prime(int n);
unsigned int hash_string(const char *str, int table_size);
url_collection_t *create_url_collection(int initial_capacity);
int url_exists(url_collection_t *collection, const char *url);
int resize_hash_table(url_collection_t *collection);
int add_to_hash(url_collection_t *collection, const char *url);
int add_url(url_collection_t *collection, const char *url);
void free_url_collection(url_collection_t *collection);
void print_url_collection(url_collection_t *collection);
int extract_image_urls(url_collection_t *collection, const char *html, const char *base_url);
int download_image(const char *url, const char *filename);
void *download_thread(void *arg);
char *extract_domain(const char *url);
void throttle_request(const char *domain);
// clang-format on

int
main (int argc, char *argv[])
{
  if (argc < 2)
  {
    printf ("Usage: %s <url>\n", argv[0]);
    return 1;
  }

  curl_global_init (CURL_GLOBAL_ALL);

  mkdir (OUTPUT_DIR, 0755);

  url_collection_t *collection = create_url_collection (0);
  if (!collection)
  {
    fprintf (stderr, "Failed to create URL collection.\n");
    return 1;
  }

  const char *url = argv[1];
  printf ("Downloading HTML from %s\n", url);

  collection->base_domain = extract_domain (url);
  if (collection->base_domain)
  {
    printf ("Base domain: %s\n", collection->base_domain);
  }

  char *html = download_html (url);

  if (!html)
  {
    fprintf (stderr, "Failed to download HTML from %s\n", url);
    free_url_collection (collection);
    curl_global_cleanup ();
    return 1;
  }

  printf ("Extracting image URLs from HTML...\n");
  int found = extract_image_urls (collection, html, url);
  printf ("Found %d unique image URLs\n", found);

  free (html);

  if (found <= 0)
  {
    printf ("No images found on the page.\n");
    free_url_collection (collection);
    curl_global_cleanup ();
    return 0;
  }

  print_url_collection (collection);

  printf ("Downloading images using %d threads...\n", NUM_THREADS);

  pthread_t threads[NUM_THREADS];
  url_index = 0;

  for (int i = 0; i < NUM_THREADS; ++i)
  {
    struct ThreadData *data = malloc (sizeof (struct ThreadData));
    if (!data)
    {
      fprintf (stderr, "Failed to allocate thread data for thread %d\n", i);
      continue;
    }
    data->collection = collection;
    data->thread_id = i;
    pthread_create (&threads[i], NULL, download_thread, data);

    usleep (100000);
  }

  for (int i = 0; i < NUM_THREADS; ++i)
  {
    pthread_join (threads[i], NULL);
  }

  printf ("All downloads complete.\n");

  free_url_collection (collection);
  curl_global_cleanup ();

  return 0;
}

static size_t
WriteCallback (void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t totalBytes = size * nmemb;
  struct MemoryChunk *chunk = (struct MemoryChunk *) userp;
  char *ptr = realloc (chunk->data, chunk->size + totalBytes + 1);
  if (ptr == NULL)
  {
    fprintf (stderr, "Out of memory while downloading HTML.\n");
    return 0;
  }
  chunk->data = ptr;
  memcpy (chunk->data + chunk->size, contents, totalBytes);
  chunk->size += totalBytes;
  chunk->data[chunk->size] = '\0';
  return totalBytes;
}

static size_t
WriteFileCallback (void *contents, size_t size, size_t nmemb, void *userp)
{
  struct FileWriterData *out = (struct FileWriterData *) userp;
  if (!out || !out->fp)
    return 0;
  size_t written = fwrite (contents, size, nmemb, out->fp);
  return written;
}

char *
extract_domain (const char *url)
{
  if (!url)
    return NULL;

  char *domain = NULL;

  if (strstr (url, "://"))
  {
    const char *proto_end = strstr (url, "://") + 3;
    const char *domain_end = strchr (proto_end, '/');

    if (domain_end)
    {
      int domain_len = domain_end - proto_end;
      domain = (char *) malloc (domain_len + 1);
      if (domain)
      {
        strncpy (domain, proto_end, domain_len);
        domain[domain_len] = '\0';
      }
    }
    else
    {
      domain = strdup (proto_end);
    }
  }

  return domain;
}

void
throttle_request (const char *domain)
{
  if (!domain)
    return;

  static char last_domain[256] = "";
  static time_t last_request_time = 0;
  time_t current_time;

  pthread_mutex_lock (&domain_mutex);

  current_time = time (NULL);

  if (strcmp (last_domain, domain) == 0)
  {
    time_t elapsed = current_time - last_request_time;
    time_t delay_sec = REQUEST_DELAY_MS / 1000;

    if (elapsed < delay_sec)
    {
      unsigned int sleep_time = (delay_sec - elapsed) * 1000000;
      pthread_mutex_unlock (&domain_mutex);
      usleep (sleep_time);
      pthread_mutex_lock (&domain_mutex);
    }
  }

  strncpy (last_domain, domain, sizeof (last_domain) - 1);
  last_domain[sizeof (last_domain) - 1] = '\0';
  last_request_time = time (NULL);

  pthread_mutex_unlock (&domain_mutex);
}

char *
download_html (const char *url)
{
  if (!url)
    return NULL;

  CURL *curl = curl_easy_init ();
  if (!curl)
  {
    fprintf (stderr, "Failed to initialize libcurl\n");
    return NULL;
  }

  char *domain = extract_domain (url);
  if (domain)
  {
    throttle_request (domain);
    free (domain);
  }

  struct MemoryChunk chunk = {0};
  chunk.data = malloc (1);
  if (!chunk.data)
  {
    curl_easy_cleanup (curl);
    return NULL;
  }

  chunk.size = 0;

  curl_easy_setopt (curl, CURLOPT_URL, url);
  curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt (curl, CURLOPT_WRITEDATA, (void *) &chunk);
  curl_easy_setopt (curl, CURLOPT_USERAGENT, "Mozilla/5.0");
  curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt (curl, CURLOPT_TIMEOUT, 30L);

  CURLcode res = curl_easy_perform (curl);

  curl_easy_cleanup (curl);

  if (res != CURLE_OK)
  {
    fprintf (stderr, "curl_easy_perform() failed: %s\n",
             curl_easy_strerror (res));
    free (chunk.data);
    return NULL;
  }

  return chunk.data;
}

int
is_prime (int n)
{
  if (n <= 1)
    return 0;
  if (n <= 3)
    return 1;
  if (n % 2 == 0 || n % 3 == 0)
    return 0;

  for (int i = 5; i * i <= n; i += 6)
  {
    if (n % i == 0 || n % (i + 2) == 0)
      return 0;
  }

  return 1;
}

unsigned int
hash_string (const char *str, int table_size)
{
  if (!str || table_size <= 0)
    return 0;

  unsigned int hash = 0;
  while (*str)
  {
    hash = hash * 31 + (*str++);
  }
  return hash % table_size;
}

url_collection_t *
create_url_collection (int initial_capacity)
{
  if (initial_capacity <= 0)
  {
    initial_capacity = 8;
  }

  url_collection_t *collection
    = (url_collection_t *) malloc (sizeof (url_collection_t));
  if (!collection)
  {
    fprintf (stderr, "Failed to allocate URL collection.\n");
    return NULL;
  }

  memset (collection, 0, sizeof (url_collection_t));

  collection->urls = (char **) malloc (initial_capacity * sizeof (char *));
  if (!collection->urls)
  {
    fprintf (stderr, "Failed to allocate memory for URL array.\n");
    free (collection);
    return NULL;
  }

  collection->capacity = initial_capacity;

  collection->hash_table
    = (hash_entry_t **) calloc (INITIAL_HASH_SIZE, sizeof (hash_entry_t *));
  if (!collection->hash_table)
  {
    fprintf (stderr, "Failed to initialize hash table.\n");
    free (collection->urls);
    free (collection);
    return NULL;
  }

  collection->hash_size = INITIAL_HASH_SIZE;
  collection->base_domain = NULL;

  return collection;
}

int
url_exists (url_collection_t *collection, const char *url)
{
  if (!collection || !collection->hash_table || !url)
  {
    return 0;
  }

  unsigned int hash = hash_string (url, collection->hash_size);
  hash_entry_t *entry = collection->hash_table[hash];

  while (entry != NULL)
  {
    if (strcmp (entry->url, url) == 0)
    {
      return 1;
    }
    entry = entry->next;
  }

  return 0;
}

int
resize_hash_table (url_collection_t *collection)
{
  if (!collection || !collection->hash_table)
    return 0;

  int old_size = collection->hash_size;
  hash_entry_t **old_table = collection->hash_table;

  int new_size = old_size * 2 + 1;
  while (!is_prime (new_size))
  {
    new_size += 2;
  }

  hash_entry_t **new_table
    = (hash_entry_t **) calloc (new_size, sizeof (hash_entry_t *));
  if (!new_table)
  {
    fprintf (stderr, "Failed to allocate new hash table\n");
    return 0;
  }

  for (int i = 0; i < old_size; i++)
  {
    hash_entry_t *entry = old_table[i];
    while (entry != NULL)
    {
      hash_entry_t *next = entry->next;

      unsigned int hash = hash_string (entry->url, new_size);
      entry->next = new_table[hash];
      new_table[hash] = entry;

      entry = next;
    }
  }

  collection->hash_table = new_table;
  collection->hash_size = new_size;

  free (old_table);

  return 1;
}

int
add_to_hash (url_collection_t *collection, const char *url)
{
  if (!collection || !url)
    return 0;

  double load_factor
    = (double) (collection->hash_count + 1) / collection->hash_size;
  if (load_factor > MAX_LOAD_FACTOR)
  {
    if (!resize_hash_table (collection))
    {
      return 0;
    }
  }

  hash_entry_t *entry = (hash_entry_t *) malloc (sizeof (hash_entry_t));
  if (!entry)
  {
    fprintf (stderr, "Failed to allocate hash entry\n");
    return 0;
  }

  entry->url = strdup (url);
  if (!entry->url)
  {
    fprintf (stderr, "Failed to duplicate URL for hash table\n");
    free (entry);
    return 0;
  }

  unsigned int hash = hash_string (url, collection->hash_size);
  entry->next = collection->hash_table[hash];
  collection->hash_table[hash] = entry;

  collection->hash_count++;

  return 1;
}

int
add_url (url_collection_t *collection, const char *url)
{
  if (!collection || !url)
  {
    return 0;
  }

  if (url_exists (collection, url))
  {
    printf ("Skipping duplicate URL: %s\n", url);
    return 1;
  }

  if (collection->count >= collection->capacity)
  {
    int new_capacity = collection->capacity * 2;
    char **new_urls
      = (char **) realloc (collection->urls, new_capacity * sizeof (char *));
    if (new_urls == NULL)
    {
      fprintf (stderr, "Failed to resize URL collection.\n");
      return 0;
    }
    collection->urls = new_urls;
    collection->capacity = new_capacity;
  }

  collection->urls[collection->count] = strdup (url);
  if (collection->urls[collection->count] == NULL)
  {
    fprintf (stderr, "Failed to duplicate URL string.\n");
    return 0;
  }

  if (!add_to_hash (collection, url))
  {
    free (collection->urls[collection->count]);
    return 0;
  }

  collection->count++;
  printf ("Added URL to collection: %s\n", url);

  return 1;
}

void
free_url_collection (url_collection_t *collection)
{
  if (!collection)
  {
    return;
  }

  if (collection->urls)
  {
    for (int i = 0; i < collection->count; i++)
    {
      free (collection->urls[i]);
    }
    free (collection->urls);
  }

  if (collection->hash_table)
  {
    for (int i = 0; i < collection->hash_size; i++)
    {
      hash_entry_t *entry = collection->hash_table[i];
      while (entry != NULL)
      {
        hash_entry_t *next = entry->next;
        free (entry->url);
        free (entry);
        entry = next;
      }
    }
    free (collection->hash_table);
  }

  if (collection->base_domain)
  {
    free (collection->base_domain);
  }

  free (collection);
}

void
print_url_collection (url_collection_t *collection)
{
  if (!collection)
  {
    printf ("No collection to print.\n");
    return;
  }

  printf ("\nCollected Image URLs (%d):\n", collection->count);
  printf ("------------------------------------------------\n");

  for (int i = 0; i < collection->count; i++)
  {
    printf ("[%d] %s\n", i + 1, collection->urls[i]);
  }

  printf ("------------------------------------------------\n");
}

int
extract_image_urls (url_collection_t *collection, const char *html,
                    const char *base_url)
{
  if (!collection || !html || !base_url)
  {
    fprintf (stderr, "Invalid parameters for extract_image_urls\n");
    return -1;
  }

  int initial_count = collection->count;

  xmlInitParser ();

  htmlDocPtr doc = htmlReadMemory (html, strlen (html), base_url, NULL,
                                   HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
  if (doc == NULL)
  {
    fprintf (stderr, "Failed to parse HTML.\n");
    xmlCleanupParser ();
    return -1;
  }

  xmlXPathContextPtr xpath_ctx = xmlXPathNewContext (doc);
  if (xpath_ctx == NULL)
  {
    fprintf (stderr, "Failed to create XPath context.\n");
    xmlFreeDoc (doc);
    xmlCleanupParser ();
    return -1;
  }

  xmlXPathObjectPtr xpath_obj
    = xmlXPathEvalExpression ((const xmlChar *) "//img/@src", xpath_ctx);

  if (xpath_obj == NULL)
  {
    fprintf (stderr, "Failed to evaluate XPath expression.\n");
    xmlXPathFreeContext (xpath_ctx);
    xmlFreeDoc (doc);
    xmlCleanupParser ();
    return -1;
  }

  xmlNodeSetPtr nodes = xpath_obj->nodesetval;

  if (nodes)
  {
    for (int i = 0; i < nodes->nodeNr; i++)
    {
      xmlNodePtr node = nodes->nodeTab[i];

      if (node->type == XML_ATTRIBUTE_NODE)
      {
        xmlChar *src = xmlNodeGetContent (node);

        if (src)
        {
          if (base_url && !strstr ((char *) src, "://"))
          {
            xmlChar *full_url = NULL;

            if (src[0] == '/')
            {
              char *domain_end = NULL;
              char domain[1024] = {0};

              if (strstr (base_url, "://"))
              {
                const char *proto_end = strstr (base_url, "://") + 3;
                domain_end = strchr (proto_end, '/');

                if (domain_end)
                {
                  int domain_len = domain_end - base_url;
                  strncpy (domain, base_url, domain_len);
                  domain[domain_len] = '\0';
                }
                else
                {
                  strcpy (domain, base_url);
                }

                char *full_path = (char *) malloc (strlen (domain)
                                                   + strlen ((char *) src) + 1);

                if (full_path)
                {
                  sprintf (full_path, "%s%s", domain, (char *) src);
                  add_url (collection, full_path);
                  free (full_path);
                }
              }
              else
              {
                add_url (collection, (char *) src);
              }
            }
            else
            {
              full_url = xmlBuildURI (src, (const xmlChar *) base_url);

              if (full_url)
              {
                add_url (collection, (char *) full_url);
                xmlFree (full_url);
              }
              else
              {
                add_url (collection, (char *) src);
              }
            }
          }
          else
          {
            add_url (collection, (char *) src);
          }

          xmlFree (src);
        }
      }
    }
  }

  xmlXPathFreeObject (xpath_obj);
  xmlXPathFreeContext (xpath_ctx);
  xmlFreeDoc (doc);
  xmlCleanupParser ();

  return collection->count - initial_count;
}

int
download_image (const char *url, const char *filename)
{
  if (!url || !filename)
    return -1;

  CURL *curl = curl_easy_init ();
  if (!curl)
    return -1;

  char *domain = extract_domain (url);
  if (domain)
  {
    throttle_request (domain);
    free (domain);
  }

  FILE *fp = fopen (filename, "wb");
  if (!fp)
  {
    curl_easy_cleanup (curl);
    return -1;
  }

  struct FileWriterData writer = {.fp = fp};

  curl_easy_setopt (curl, CURLOPT_URL, url);
  curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
  curl_easy_setopt (curl, CURLOPT_WRITEDATA, &writer);
  curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt (curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt (curl, CURLOPT_USERAGENT, "Mozilla/5.0");

  CURLcode res = curl_easy_perform (curl);

  fclose (fp);
  curl_easy_cleanup (curl);

  if (res != CURLE_OK)
  {
    fprintf (stderr, "Failed to download %s: %s\n", url,
             curl_easy_strerror (res));
    remove (filename);
    return -1;
  }

  return 0;
}

void *
download_thread (void *arg)
{
  if (!arg)
    return NULL;

  url_collection_t *collection = ((struct ThreadData *) arg)->collection;
  int thread_id = ((struct ThreadData *) arg)->thread_id;

  while (1)
  {
    pthread_mutex_lock (&url_mutex);
    int idx = url_index++;
    pthread_mutex_unlock (&url_mutex);

    if (idx >= collection->count)
      break;

    const char *img_url = collection->urls[idx];
    char filename[256];

    const char *ext = "jpg";
    const char *dot = strrchr (img_url, '.');
    if (dot && strlen (dot) <= 5)
    {
      ext = dot + 1;
    }

    snprintf (filename, sizeof (filename), "%s/img_%d_%d.%s", OUTPUT_DIR,
              thread_id, idx, ext);

    printf ("Thread %d downloading: %s -> %s\n", thread_id, img_url, filename);

    download_image (img_url, filename);

    usleep (DOWNLOAD_DELAY_MS * 1000);
  }

  free (arg);
  return NULL;
}
