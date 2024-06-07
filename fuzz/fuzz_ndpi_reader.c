#include "reader_util.h"
#include "ndpi_api.h"
#include "fuzz_common_code.h"

#include <pcap/pcap.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

struct ndpi_workflow_prefs *prefs = NULL;
struct ndpi_workflow *workflow = NULL;
struct ndpi_global_context *g_ctx;

u_int8_t enable_payload_analyzer = 0;
u_int8_t enable_flow_stats = 1;
u_int8_t human_readeable_string_len = 5;
u_int8_t max_num_udp_dissected_pkts = 16 /* 8 is enough for most protocols, Signal requires more */, max_num_tcp_dissected_pkts = 80 /* due to telnet */;
int malloc_size_stats = 0;

extern void ndpi_report_payload_stats(FILE *out);

#ifdef CRYPT_FORCE_NO_AESNI
extern int force_no_aesni;
#endif

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
  pcap_t * pkts;
  const u_char *pkt;
  struct pcap_pkthdr *header;
  int r;
  char errbuf[PCAP_ERRBUF_SIZE];
  NDPI_PROTOCOL_BITMASK all;
  u_int i;
  FILE *fd;

  if (prefs == NULL) {
    prefs = calloc(sizeof(struct ndpi_workflow_prefs), 1);
    if (prefs == NULL) {
      //should not happen
      return 1;
    }
    prefs->decode_tunnels = 1;
    prefs->num_roots = 16;
    prefs->max_ndpi_flows = 16 * 1024 * 1024;
    prefs->quiet_mode = 0;

#ifdef ENABLE_MEM_ALLOC_FAILURES
    fuzz_set_alloc_callbacks();
#endif

    g_ctx = ndpi_global_init();

    workflow = ndpi_workflow_init(prefs, NULL /* pcap handler will be set later */, 0, ndpi_serialization_format_json, g_ctx);

    ndpi_set_config(workflow->ndpi_struct, NULL, "log.level", "3");
    ndpi_set_config(workflow->ndpi_struct, "all", "log", "1");

    ndpi_load_domain_suffixes(workflow->ndpi_struct, "public_suffix_list.dat");
    ndpi_load_categories_dir(workflow->ndpi_struct, "./lists/");
    ndpi_load_protocols_file(workflow->ndpi_struct, "protos.txt");
    ndpi_load_categories_file(workflow->ndpi_struct, "categories.txt", NULL);
    ndpi_load_risk_domain_file(workflow->ndpi_struct, "risky_domains.txt");
    ndpi_load_malicious_ja3_file(workflow->ndpi_struct, "ja3_fingerprints.csv");
    ndpi_load_malicious_sha1_file(workflow->ndpi_struct, "sha1_fingerprints.csv");

    // enable all protocols
    NDPI_BITMASK_SET_ALL(all);
    ndpi_set_protocol_detection_bitmask2(workflow->ndpi_struct, &all);

    ndpi_set_config(workflow->ndpi_struct, NULL, "flow.track_payload", "1");
    ndpi_set_config(workflow->ndpi_struct, NULL, "tcp_ack_payload_heuristic", "1");
    ndpi_set_config(workflow->ndpi_struct, "tls", "application_blocks_tracking", "1");
    ndpi_set_config(workflow->ndpi_struct, "stun", "max_packets_extra_dissection", "255");
    ndpi_set_config(workflow->ndpi_struct, "rtp", "search_for_stun", "1");

    ndpi_finalize_initialization(workflow->ndpi_struct);

#ifdef CRYPT_FORCE_NO_AESNI
    force_no_aesni = 1;
#endif

#ifdef ENABLE_PAYLOAD_ANALYZER
   enable_payload_analyzer = 1;
#endif
  }

#ifdef ENABLE_MEM_ALLOC_FAILURES
  /* Don't fail memory allocations until init phase is done */
  fuzz_set_alloc_callbacks_and_seed(Size);
#endif

  fd = buffer_to_file(Data, Size);
  if (fd == NULL)
    return 0;

  pkts = pcap_fopen_offline(fd, errbuf);
  if (pkts == NULL) {
    fclose(fd);
    return 0;
  }
  if (ndpi_is_datalink_supported(pcap_datalink(pkts)) == 0)
  {
    /* Do not fail if the datalink type is not supported (may happen often during fuzzing). */
    pcap_close(pkts);
    return 0;
  }

  workflow->pcap_handle = pkts;
  /* Init flow tree */
  workflow->ndpi_flows_root = ndpi_calloc(workflow->prefs.num_roots, sizeof(void *));
  if(!workflow->ndpi_flows_root) {
    pcap_close(pkts);
    return 0;
  }

  header = NULL;
  r = pcap_next_ex(pkts, &header, &pkt);
  while (r > 0) {
    /* allocate an exact size buffer to check overflows */
    uint8_t *packet_checked = malloc(header->caplen);

    if(packet_checked) {
      ndpi_risk flow_risk;

      memcpy(packet_checked, pkt, header->caplen);
      ndpi_workflow_process_packet(workflow, header, packet_checked, &flow_risk);
      free(packet_checked);
    }

    r = pcap_next_ex(pkts, &header, &pkt);
  }
  pcap_close(pkts);

  /* Free flow trees */
  for(i = 0; i < workflow->prefs.num_roots; i++)
    ndpi_tdestroy(workflow->ndpi_flows_root[i], ndpi_flow_info_freer);
  ndpi_free(workflow->ndpi_flows_root);
  /* Free payload analyzer data */
  if(enable_payload_analyzer)
    ndpi_report_payload_stats(stdout);

#ifdef ENABLE_PAYLOAD_ANALYZER
  ndpi_update_params(SPLT_PARAM_TYPE, "splt_param.txt");
  ndpi_update_params(BD_PARAM_TYPE, "bd_param.txt");
  ndpi_update_params(2, ""); /* invalid */
#endif

  return 0;
}
