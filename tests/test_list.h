/* The full test list, shared by the host and ESP-IDF runners. */
#ifndef EPI_TEST_LIST_H
#define EPI_TEST_LIST_H

/* Runs every test between UNITY_BEGIN and UNITY_END; returns the failure count. */
int epi_run_all_tests(void);

#endif
