#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <curl/curl.h>
#include <termios.h>

#ifdef __APPLE__
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <openssl/crypto.h>

#include "basic_defs.h"

#include "block-store.h"
#include "peergroup.h"
#include "poolworker.h"
#include "util.h"
#include "hashtable.h"
#include "wallet.h"
#include "config.h"
#include "poll.h"
#include "netasync.h"
#include "key.h"
#include "bloom.h"
#include "addrbook.h"
#include "serialize.h"
#include "file.h"
#include "bitc.h"
#include "buff.h"
#include "test.h"
#include "ncui.h"
#include "base58.h"
#include "ip_info.h"
#include "crypt.h"
#include "bitc_ui.h"


#define LGPFX "BITC:"


enum btc_req_type {
   BTC_REQ_STOP,
   BTC_REQ_TX,
};

struct btc_req {
   struct circlist_item  item;
   enum btc_req_type     type;
   void                 *clientData;
};

static void bitc_sigint_handler(int sig);

static struct BITCApp theBitcApp;
struct BITCApp *btc = &theBitcApp;
bool bitc_testing = 0;


/*
 *---------------------------------------------------------------------
 *
 * bitc_signal_int_default --
 *
 *---------------------------------------------------------------------
 */

static void
bitc_signal_int_default(void)
{
   signal(SIGINT, SIG_DFL);
}


/*
 *---------------------------------------------------------------------
 *
 * bitc_signal_handler --
 *
 *---------------------------------------------------------------------
 */

static void
bitc_signal_handler(int sig)
{
   Panic("Unexpected signal %u received.\n", sig);
}


/*
 *---------------------------------------------------------------------
 *
 * bitc_signal_install --
 *
 *---------------------------------------------------------------------
 */

static void
bitc_signal_install(void)
{
   signal(SIGINT, bitc_sigint_handler);

   signal(SIGSEGV, bitc_signal_handler);
   signal(SIGBUS,  bitc_signal_handler);
   signal(SIGILL,  bitc_signal_handler);

   signal(SIGPIPE, SIG_IGN);
   signal(SIGHUP,  SIG_IGN);
}


/*
 *---------------------------------------------------------------------
 *
 * bitc_get_password --
 *
 *---------------------------------------------------------------------
 */

static struct secure_area *
bitc_get_password(void)
{
   struct secure_area *sec;
   struct termios old;
   struct termios new;
   int res;

   res = tcgetattr(STDIN_FILENO, &old);
   ASSERT(res == 0);
   new = old;
   new.c_lflag &= ~ECHO;
   res = tcsetattr(STDIN_FILENO, TCSANOW, &new);
   ASSERT(res == 0);

   sec = secure_alloc(256);
   sec->len = 0;

   bitc_signal_int_default();

   while (btc->stop == 0) {
      char c = getchar();
      if (c == '\n' || c == '\r') {
         break;
      }
      if (c == '\b') {
         if (sec->len > 0) {
            sec->len--;
            sec->buf[sec->len] = '\0';
         }
         continue;
      }
      sec->buf[sec->len] = c;
      sec->len++;
      ASSERT(sec->len < sec->alloc_len);
   }
   bitc_signal_install();

   sec->buf[sec->len] = '\0';
   sec->len++;

   tcsetattr(STDIN_FILENO, TCSANOW, &old);

   return sec;
}


/*
 *---------------------------------------------------------------------
 *
 * bitc_encrypt_wallet --
 *
 *---------------------------------------------------------------------
 */

static int
bitc_encrypt_wallet(struct secure_area *pass_old,
                    struct secure_area *pass_new)
{
   char *errStr = NULL;
   int res;

   if (pass_new == NULL) {
      Warning("You need to specify a password.\n");
      return 1;
   }

   printf("Encrypting wallet with passphrase..\n");
   res = blockstore_init(btc->config, &btc->blockStore);
   ASSERT(res == 0);
   res = wallet_open(btc->config, pass_old, &errStr, &btc->wallet);
   ASSERT(res == 0);

   res = wallet_encrypt(btc->wallet, pass_new);
   if (res) {
      Warning("Failed to encrypt wallet.\n");
   }

   wallet_close(btc->wallet);
   blockstore_exit(btc->blockStore);

   return res;
}


/*
 *---------------------------------------------------------------------
 *
 * bitc_add_address --
 *
 *---------------------------------------------------------------------
 */

static int
bitc_add_address(const char         *desc,
                 struct secure_area *pass,
                 char              **btc_addr)
{
   char *errStr = NULL;
   int res;

   res = blockstore_init(btc->config, &btc->blockStore);
   ASSERT(res == 0);
   res = wallet_open(btc->config, pass, &errStr, &btc->wallet);
   ASSERT(res == 0);

   res = wallet_add_key(btc->wallet, desc, btc_addr);
   if (res) {
      Warning("Failed to add address to wallet.\n");
   }

   wallet_close(btc->wallet);
   blockstore_exit(btc->blockStore);

   return res;
}


/*
 *---------------------------------------------------------------------
 *
 * btc_bye --
 *
 *---------------------------------------------------------------------
 */

static void
btc_bye(void)
{
   printf("Contribute! https://github.com/bit-c/bitc\n");
}


/*
 *---------------------------------------------------------------------
 *
 * btc_version_and_exit --
 *
 *---------------------------------------------------------------------
 */

static void
btc_version_and_exit(void)
{
   printf("btc: %s.\n"
          " - compiled on %s at %s,\n"
          " - version: %s.\n",
          BTC_CLIENT_DESC,
          __DATE__, __TIME__,
          BTC_CLIENT_VERSION);

   exit(0);
}


/*
 *---------------------------------------------------------------------
 *
 * btc_usage --
 *
 *---------------------------------------------------------------------
 */

static void
btc_usage(void)
{
   printf("bitc: %s.\n"
          "Options:\n"
          " -a, --address                  generate new address, add to wallet\n"
          " -c, --config     <configPath>  config file to use, default: ~/.bitc/main.cfg\n"
          " -d, --daemon                   daemon mode: no-ui\n"
          " -h, --help                     show this help message\n"
          " -e, --encrypt                  encrypt the wallet file\n"
          " -n, --numPeers   <maxPeers>    number of peers to connect to, default is 5\n"
          " -p, --passphrase               prompt for passphrase\n"
          " -t, --test       <param>       test suite: argument is the name of the test.\n"
          " -u, --update                   update block-store and exit\n"
          " -v, --version                  display version string and exit\n"
          " -z, --zap                      zap headers & txdb (wallet is preserved)\n",
          BTC_CLIENT_DESC);
}


/*
 *----------------------------------------------------------------
 *
 * btc_req_notify --
 *
 *----------------------------------------------------------------
 */

static void
btc_req_notify(void)
{
   uint8 val = 1;
   ssize_t res;

   res = write(btc->notifyFd, &val, sizeof val);
   ASSERT(res == 1);
}


/*
 *---------------------------------------------------------------------
 *
 * bitc_sigint_handler --
 *
 *---------------------------------------------------------------------
 */

static void
bitc_sigint_handler(int sig)
{
   if (sig == SIGINT) {
      Warning("CTRL-C received. Exiting..\n");
   } else {
      Warning("Signal %u received. Exiting..\n", sig);
   }
   btc->stop = 2;
   btc_req_notify();
}


/*
 *---------------------------------------------------------------------
 *
 * btc_log_sslversion --
 *
 *---------------------------------------------------------------------
 */

static void
btc_log_sslversion(void)
{
   const char *sslVersion = SSLeay_version(SSLEAY_VERSION);
   Log("Using %s.\n", sslVersion);
}


/*
 *------------------------------------------------------------------------
 *
 * btc_check_create_file --
 *
 *------------------------------------------------------------------------
 */

static int
btc_check_create_file(const char *filename,
                      const char *label)
{
   int res;

   if (file_exists(filename)) {
      return 0;
   }

   Log(LGPFX" creating %s file: %s\n", label, filename);
   res = file_create(filename);
   if (res) {
      printf("Failed to create %s file '%s': %s\n",
             label, filename, strerror(res));
      return res;
   }
   res = file_chmod(filename, 0600);
   if (res) {
      printf("Failed to chmod 0600 %s file '%s': %s\n",
             label, filename, strerror(res));
   }
   return res;
}


/*
 *------------------------------------------------------------------------
 *
 * btc_check_config --
 *
 *------------------------------------------------------------------------
 */

static int
btc_check_config(void)
{
   char *btcDir;
   char *cfgPath;
   char *ctcPath;
   char *txPath;
   char *home;
   int res = 0;

   home = util_gethomedir();
   btcDir  = safe_asprintf("%s/.bitc/", home);
   cfgPath = safe_asprintf("%s/.bitc/main.cfg", home);
   ctcPath = safe_asprintf("%s/.bitc/contacts.cfg", home);
   txPath  = safe_asprintf("%s/.bitc/tx-labels.cfg", home);

   if (!file_exists(btcDir) || !file_exists(cfgPath)) {
      printf("\nIt looks like you're a new user. Welcome!\n"
             "\n"
             "Note that btc uses the directory: ~/.bitc to store:\n"
             " - block headers:        ~/.bitc/headers.dat     -- ~ 20 MB\n"
             " - peer IP addresses:    ~/.bitc/peers.dat       --  ~ 2 MB\n"
             " - transaction database: ~/.bitc/txdb            --  < 1 MB\n"
             " - wallet keys:          ~/.bitc/wallet.cfg      --  < 1 KB\n"
             " - main config file:     ~/.bitc/main.cfg        --  < 1 KB\n"
             " - a contacts file:      ~/.bitc/contacts.cfg    --  < 1 KB\n"
             " - a tx-label file:      ~/.bitc/tx-labels.cfg   --  < 1 KB\n\n");
   }

   if (!file_exists(btcDir)) {
      Log(LGPFX" creating directory: %s\n", btcDir);
      res = file_mkdir(btcDir);
      if (res) {
         printf("Failed to create directory '%s': %s\n",
                btcDir, strerror(res));
         goto exit;
      }
      res = file_chmod(btcDir, 0700);
      if (res) {
         printf("Failed to chmod 0600 directory '%s': %s\n",
                btcDir, strerror(res));
         goto exit;
      }
   }
   btc_check_create_file(cfgPath, "config");
   btc_check_create_file(txPath, "tx-labels");

   if (!file_exists(ctcPath)) {
      struct config *cfg;

      btc_check_create_file(ctcPath, "contacts");

      cfg = config_create();
      config_setstring(cfg, "1PBP4S44b1ro3kD6LQhBYnsF3fAp1HYPf2", "contact0.addr");
      config_setstring(cfg, "Support bitc development -- https://bit-c.github.com",
                       "contact0.label");

      config_setstring(cfg, "1PC9aZC4hNX2rmmrt7uHTfYAS3hRbph4UN", "contact1.addr");
      config_setstring(cfg, "Free Software Foundation -- https://fsf.org/donate/",
                       "contact1.label");
      config_setstring(cfg, "1BTCorgHwCg6u2YSAWKgS17qUad6kHmtQW", "contact2.addr");
      config_setstring(cfg, "Bitcoin Foundation -- https://bitcoinfoundation.org/donate",
                       "contact2.label");

      config_setstring(cfg, "3", "contacts.numEntries");

      res = config_write(cfg, ctcPath);
      if (res) {
         printf("Failed to save contacts file: %s\n", strerror(res));
      }
   }

exit:
   free(txPath);
   free(cfgPath);
   free(ctcPath);
   free(btcDir);
   free(home);

   return res;
}


/*
 *----------------------------------------------------------------
 *
 * btc_check_wallet --
 *
 *----------------------------------------------------------------
 */

static int
btc_check_wallet(void)
{
   char *btc_addr = NULL;
   char *wltPath;
   int res = 0;

   wltPath = wallet_get_filename();

   if (file_exists(wltPath)) {
      goto exit;
   }

   res = bitc_add_address("Main address @bitc", NULL, &btc_addr);
   if (res) {
      printf("Failed to add key to wallet: %s\n", strerror(res));
      goto exit;
   }
   res = 1;

   printf("Your bitcoin address with this app:\n\n"
          "     %s\n\n", btc_addr);
   printf("Type a key to continue...\n");
   free(btc_addr);

   printf("To navigate the UI:\n"
          " - <left> and <right> allow you to change panel,\n"
          " - <CTRL> + T to initiate a transaction,\n"
          " - type 'q' to exit.\n\n");

   printf("Your install of bitc is ready to go.\n");

exit:
   free(wltPath);
   return res;
}


/*
 *----------------------------------------------------------------
 *
 * btc_load_misc_config --
 *
 *----------------------------------------------------------------
 */

static void
btc_load_misc_config(void)
{
   char *defaultPath;
   char *home;
   char *path;
   int res;

   home = util_gethomedir();

   /*
    * contacts.
    */
   defaultPath = safe_asprintf("%s/.bitc/contacts.cfg", home);
   path = config_getstring(btc->config, defaultPath, "contacts.filename");
   res = config_load(path, &btc->contactsCfg);
   if (res) {
      Warning("Please create a minimal config: %s\n", path);
   }
   free(defaultPath);
   free(path);

   /*
    * tx-label.
    */
   defaultPath = safe_asprintf("%s/.bitc/tx-labels.cfg", home);
   path = config_getstring(btc->config, defaultPath, "tx-labels.filename");
   res = config_load(path, &btc->txLabelsCfg);
   if (res) {
      Warning("Please create a minimal config: %s\n", path);
   }
   free(defaultPath);
   free(path);

   free(home);
}


/*
 *----------------------------------------------------------------
 *
 * btc_load_config --
 *
 *----------------------------------------------------------------
 */

static int
btc_load_config(struct config **config,
                const char     *configPath)
{
   char *defaultPath = NULL;
   const char *path;
   int res;

   if (configPath == NULL) {
      char *home = util_gethomedir();
      defaultPath = safe_asprintf("%s/.bitc/main.cfg", home);
      free(home);
      path = defaultPath;
   } else {
      path = configPath;
   }
   res = config_load(path, config);
   if (res) {
      Warning("Please create a minimal config: %s\n", path);
   }
   free(defaultPath);
   return res;
}


/*
 *----------------------------------------------------------------
 *
 * btc_filter_init --
 *
 *----------------------------------------------------------------
 */

static void
btc_filter_init(void)
{
   struct bloom_filter *f;

   f = bloom_create(10, 0.001);

   wallet_update_filter(btc->wallet, f);

   btc->filter = f;
}


/*
 *----------------------------------------------------------------
 *
 * btc_poll_exit --
 *
 *----------------------------------------------------------------
 */

static void
btc_poll_exit(void)
{
   poll_destroy(btc->poll);
   btc->poll = NULL;
}


/*
 *----------------------------------------------------------------
 *
 * btc_poll_init --
 *
 *----------------------------------------------------------------
 */

static void
btc_poll_init(void)
{
   btc->poll = poll_create();
}


/*
 *----------------------------------------------------------------
 *
 * btc_req_enqueue --
 *
 *----------------------------------------------------------------
 */

static void
btc_req_enqueue(struct btc_req *req)
{
   ASSERT(req);
   ASSERT(btc->lock);

   mutex_lock(btc->lock);
   circlist_queue_item(&btc->reqList, &req->item);
   mutex_unlock(btc->lock);

   btc_req_notify();
}


/*
 *----------------------------------------------------------------
 *
 * btc_req_alloc --
 *
 *----------------------------------------------------------------
 */

static struct btc_req *
btc_req_alloc(enum btc_req_type type)
{
   struct btc_req*req;

   req = safe_malloc(sizeof *req);
   circlist_init_item(&req->item);
   req->clientData = NULL;
   req->type       = type;

   return req;
}


/*
 *----------------------------------------------------------------
 *
 * btc_req_tx --
 *
 *----------------------------------------------------------------
 */

void
btc_req_tx(struct btc_tx_desc *tx)
{
   struct btc_req *req;

   Log(LGPFX" requesting tx: %.8f BTC to %s.\n",
       tx->total_value / ONE_BTC, tx->dst[0].addr);
   req = btc_req_alloc(BTC_REQ_TX);
   req->clientData = tx;
   btc_req_enqueue(req);
}


/*
 *----------------------------------------------------------------
 *
 * btc_req_stop --
 *
 *----------------------------------------------------------------
 */

void
btc_req_stop(void)
{
   struct btc_req *req;

   Log(LGPFX" requesting exit.\n");
   /*
    * A bit of a hack. Let's set 'btc->stop' from the UI possibly, that way
    * lengthy functions can check this value if they need to abort their
    * processing. Cf blockstore_load_etc.
    */
   btc->stop = 1;
   req = btc_req_alloc(BTC_REQ_STOP);
   btc_req_enqueue(req);
}


/*
 *----------------------------------------------------------------
 *
 * btc_transmit_tx --
 *
 *----------------------------------------------------------------
 */

static void
btc_transmit_tx(struct btc_tx_desc *tx_desc)
{
   struct btc_msg_tx tx;
   int res;

   ASSERT(tx_desc);
   btc_msg_tx_init(&tx);

   Log(LGPFX" sending %.8f BTC to %s\n",
       tx_desc->total_value / ONE_BTC, tx_desc->dst[0].addr);

   if (tx_desc->fee == -1) {
      tx_desc->fee = config_getint64(btc->config, 10000, "wallet.fee");
   }

   /*
    * Let's prevent stupid mistakes.
    */
   ASSERT(tx_desc->fee <= 100000);

   res = wallet_craft_tx(btc->wallet, tx_desc, &tx);
   if (res) {
      bitcui_set_status("TX failed: insufficient funds");
   }

   btc_msg_tx_free(&tx);
}


/*
 *----------------------------------------------------------------
 *
 * btc_process_events --
 *
 *----------------------------------------------------------------
 */

static void
btc_process_events(void)
{
   /*
    * If we got a CTRL-C, btc->stop is set to 2. We transition automatically to
    * BITC_STATE_EXITING.
    */
   if (btc->stop == 2) {
      Log(LGPFX" %s -- BITC_STATE_EXITING (CTRL-C)\n", __FUNCTION__);
      btc->state = BITC_STATE_EXITING;
   }

   while (!circlist_empty(btc->reqList)) {
      struct circlist_item *li = btc->reqList;
      struct btc_req *req;

      req = CIRCLIST_CONTAINER(li, struct btc_req, item);
      circlist_delete_item(&btc->reqList, li);
      Log(LGPFX" handling msg %d\n", req->type);

      switch (req->type) {
      case BTC_REQ_STOP:
         Log(LGPFX" %s -- BITC_STATE_EXITING.\n", __FUNCTION__);
         btc->stop = 1;
         btc->state = BITC_STATE_EXITING;
         break;
      case BTC_REQ_TX:
         Log(LGPFX" %s -- initiating tx.\n", __FUNCTION__);
         struct btc_tx_desc *tx_desc = req->clientData;
         btc_transmit_tx(tx_desc);
         free(tx_desc);
         break;
      default:
         Warning(LGPFX" unhandled btc msg %d\n", req->type);
         break;
      }

      free(req);
   }
}


/*
 *----------------------------------------------------------------
 *
 * btc_req_cb --
 *
 *----------------------------------------------------------------
 */

static void
btc_req_cb(void *clientData)
{
   ssize_t res;

   do {
      uint8 val;

      res = read(btc->eventFd, &val, sizeof val);
   } while (res > 0);

   ASSERT(res == 0 || errno == EAGAIN);

   mutex_lock(btc->lock);

   btc_process_events();

   mutex_unlock(btc->lock);
}


/*
 *----------------------------------------------------------------
 *
 * btc_req_exit --
 *
 *----------------------------------------------------------------
 */

static void
btc_req_exit(void)
{
   bool s;

   if (btc->notifyInit == 0) {
      return;
   }

   s = poll_callback_device_remove(btc->poll, btc->eventFd, 1, 0, 1,
                                   btc_req_cb, NULL);
   ASSERT(s);

   close(btc->eventFd);
   close(btc->notifyFd);

   btc->eventFd = -1;
   btc->notifyFd = -1;
}


/*
 *----------------------------------------------------------------
 *
 * btc_req_init --
 *
 *----------------------------------------------------------------
 */

static int
btc_req_init(void)
{
   int fd[2];
   int flags;
   int res;

   res = pipe(fd);
   if (res != 0) {
      res = errno;
      Log(LGPFX" Failed to create pipe: %s\n", strerror(res));
      return res;
   }
   btc->eventFd  = fd[0];
   btc->notifyFd = fd[1];

   flags = fcntl(btc->eventFd, F_GETFL, 0);
   if (flags < 0) {
      NOT_TESTED();
      return flags;
   }

   res = fcntl(btc->eventFd, F_SETFL, flags | O_NONBLOCK);
   if (res < 0) {
      NOT_TESTED();
      return res;
   }
   poll_callback_device(btc->poll, btc->eventFd, 1, 0, 1, btc_req_cb, NULL);
   btc->notifyInit = 1;

   return 0;
}


/*
 *----------------------------------------------------------------
 *
 * btc_init --
 *
 *----------------------------------------------------------------
 */

static int
btc_init(struct secure_area *passphrase,
         bool                updateAndExit,
         int                 maxPeers,
         int                 minPeersInit,
         char              **errStr)
{
   int res;

   curl_global_init(CURL_GLOBAL_DEFAULT);
   Log(LGPFX" %s -- BITC_STATE_STARTING.\n", __FUNCTION__);
   btc->state = BITC_STATE_STARTING;
   btc->wallet_state = WALLET_UNKNOWN;
   btc->updateAndExit = updateAndExit;

   bitcui_set_status("starting..");
   util_bumpnofds();
   btc_log_sslversion();
   btc_poll_init();
   btc_req_init();
   netasync_init(btc->poll);

   bitcui_set_status("loading addrbook..");
   addrbook_open(btc->config, &btc->book);

   bitcui_set_status("opening blockstore..");
   res = blockstore_init(btc->config, &btc->blockStore);
   if (res) {
      *errStr = "Failed to open block-store.";
      return res;
   }

   peergroup_init(btc->config, maxPeers, minPeersInit, 15 * 1000 * 1000); // 15 sec

   bitcui_set_status("loading wallet..");
   res = wallet_open(btc->config, passphrase, errStr, &btc->wallet);
   if (res != 0) {
      return res;
   }
   btc_filter_init();

   bitcui_set_status("adding peers..");
   peergroup_seed();

   return res;
}


/*
 *----------------------------------------------------------------
 *
 * btc_exit --
 *
 *----------------------------------------------------------------
 */

static void
btc_exit(void)
{
   Log(LGPFX" %s\n", __FUNCTION__);
   peergroup_exit(btc->peerGroup);
   btc->peerGroup = NULL;
   addrbook_close(btc->book);
   btc->book = NULL;
   bloom_free(btc->filter);
   btc->filter = NULL;
   wallet_close(btc->wallet);
   btc->wallet = NULL;
   blockstore_exit(btc->blockStore);
   btc->blockStore = NULL;
   btc_req_exit();
   netasync_exit();
   btc_poll_exit();
   curl_global_cleanup();

   config_free(btc->txLabelsCfg);
   config_free(btc->contactsCfg);
   config_free(btc->config);
}


/*
 *---------------------------------------------------------------------
 *
 * btc_daemon --
 *
 *---------------------------------------------------------------------
 */

static void
btc_daemon(bool updateAndExit,
           int maxPeers)
{
   Warning(LGPFX" daemon running.\n");
   bitcui_set_status("connecting to peers..");
   peergroup_refill(TRUE /* init */);

   while (btc->stop == 0) {
      poll_runloop(btc->poll, &btc->stop);
   }
   Warning(LGPFX" daemon stopped.\n");
}


/*
 *---------------------------------------------------------------------
 *
 * main --
 *
 *---------------------------------------------------------------------
 */

int main(int argc, char *argv[])
{
   struct secure_area *passphrase = NULL;
   const int minPeersInit = 50;
   char *addr_label = NULL;
   char *errStr = NULL;
   char *configPath = NULL;
   char *testStr = NULL;
   int maxPeers = 5;
   bool updateAndExit = 0;
   bool zap = 0;
   bool withui = 1;
   bool encrypt = 0;
   bool getpass = 0;
   int res = 0;
   char c;

   static const struct option long_opts [] = {
      { "address",      no_argument,        0,  'a' },
      { "config",       required_argument,  0,  'c' },
      { "daemon",       no_argument,        0,  'd' },
      { "encrypt",      no_argument,        0,  'e' },
      { "help",         no_argument,        0,  'h' },
      { "numPeers",     required_argument,  0,  'n' },
      { "passphrase",   required_argument,  0,  'p' },
      { "test",         required_argument,  0,  't' },
      { "update",       no_argument,        0,  'u' },
      { "version",      no_argument,        0,  'v' },
      { "zap",          no_argument,        0,  'z' },
   };

   bitc_signal_install();

   while ((c = getopt_long(argc, argv, "a:c:dehn:pt:uvz",
                           long_opts, NULL)) != EOF) {
      switch (c) {
      case 'a': addr_label = optarg;     break;
      case 'c': configPath = optarg;     break;
      case 'd': withui = 0;              break;
      case 'e': encrypt = 1;             break;
      case 'n': maxPeers = atoi(optarg); break;
      case 'p': getpass = 1;             break;
      case 't': testStr = optarg;        break;
      case 'u': updateAndExit = 1;       break;
      case 'v': btc_version_and_exit();  break;
      case 'z': zap = 1;                 break;
      case 'h':
      default:
         btc_usage();
         return 0;
      }
   }

   Log_SetLevel(1);
   {
      char *login = util_getusername();
      char *logFile;
      logFile = safe_asprintf("/tmp/bitc-%s.log", login ? login : "foo");
      Log_Init(logFile);
      free(logFile);
      free(login);
   }
   util_bumpcoresize();
   btc_check_config();

   res = btc_load_config(&btc->config, configPath);
   if (res != 0) {
      return res;
   }
   btc_load_misc_config();
   if (btc_check_wallet()) {
      return 0;
   }

   if (addr_label) {
      return bitc_add_address(addr_label, NULL, NULL);
   }

   if (testStr) {
      ASSERT(res == 0);
      res = bitc_test(testStr);
      config_free(btc->config);
      return res;
   }

   if (zap) {
      Warning(LGPFX" zap: block-store, addrbook, txdb.\n");
      blockstore_zap(btc->config);
      addrbook_zap(btc->config);
      wallet_zap_txdb(btc->config);
      peergroup_zap(btc->config);
      Warning(LGPFX" zap: done.\n");
      Log_Exit();
      return 0;
   }

   /*
    * Get the state of the wallet. One of:
    *  - plain/unencrypted,
    *  - encrypted & locked (no password or password invalid),
    *  - encrypted & unlocked: password valid.
    */
   if (!wallet_verify(NULL, &btc->wallet_state)) {
      return 0;
   }

   if (encrypt) {
      struct secure_area *pass_old = NULL;
      struct secure_area *pass_new = NULL;
      bool ok;
      bool s;

      printf("Wallet is about to be encrypted.\n");
      if (btc->wallet_state == WALLET_ENCRYPTED_LOCKED) {
         printf("   Old password: ");
         pass_old = bitc_get_password();
         printf("\n");
         s = wallet_verify(pass_old, &btc->wallet_state);
         ok = s && btc->wallet_state == WALLET_ENCRYPTED_UNLOCKED;
         if (!ok) {
            printf("Password invalid.\n");
            secure_free(pass_old);
            return 0;
         }
      }

      printf("   New password: ");
      pass_new = bitc_get_password();
      printf("\n");
      bitc_encrypt_wallet(pass_old, pass_new);
      secure_free(pass_old);
      secure_free(pass_new);
      return 0;
   }

   if (btc->wallet_state == WALLET_ENCRYPTED_LOCKED && getpass) {
      bool ok;
      bool s;

      printf("Wallet is encrypted.\n"
             "Password: ");
      passphrase = bitc_get_password();
      printf("\n");
      s = wallet_verify(passphrase, &btc->wallet_state);
      ok = s && btc->wallet_state == WALLET_ENCRYPTED_UNLOCKED;
      if (!ok) {
         printf("Password invalid.\n");
         secure_free(passphrase);
         return 0;
      }
   }

   btc->lock = mutex_alloc();
   btc->pw = poolworker_create(10);
   ipinfo_init();
   res = bitcui_start(withui);
   if (res) {
      goto exit;
   }

   res = btc_init(passphrase, updateAndExit, maxPeers, minPeersInit, &errStr);
   if (res) {
      goto exit;
   }

   btc_daemon(updateAndExit, maxPeers);

exit:
   btc_process_events();
   btc_exit();
   bitcui_stop();
   poolworker_wait(btc->pw);
   ipinfo_exit();
   poolworker_destroy(btc->pw);
   mutex_free(btc->lock);
   secure_free(passphrase);
   if (errStr) {
      printf("%s\n", errStr);
   } else {
      btc_bye();
   }

   memset(btc, 0, sizeof *btc);
   Log_Exit();

   return res;
}
