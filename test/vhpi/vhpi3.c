#include "vhpi_test.h"

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static int64_t phys_to_i64(vhpiPhysT phys)
{
   return ((uint64_t)phys.high) << 32 | phys.low;
}

static void start_of_sim(const vhpiCbDataT *cb_data)
{
   vhpi_printf("start_of_sim");

   fail_unless(phys_to_i64(vhpiFS) == 1ll);
   fail_unless(phys_to_i64(vhpiPS) == 1000ll);
   fail_unless(phys_to_i64(vhpiNS) == 1000000ll);
   fail_unless(phys_to_i64(vhpiUS) == 1000000000ll);
   fail_unless(phys_to_i64(vhpiMS) == 1000000000000ll);
   fail_unless(phys_to_i64(vhpiS) == 1000000000000000ll);
   fail_unless(phys_to_i64(vhpiMN) == 1000000000000000ll * 60);
   fail_unless(phys_to_i64(vhpiHR) == 1000000000000000ll * 60 * 60);

   vhpiPhysT res_limit = VHPI_CHECK(vhpi_get_phys(vhpiResolutionLimitP, NULL));
   fail_unless(phys_to_i64(res_limit) == phys_to_i64(vhpiFS));

   vhpiHandleT root = VHPI_CHECK(vhpi_handle(vhpiRootInst, NULL));
   fail_if(root == NULL);

   vhpiHandleT handle_x = VHPI_CHECK(vhpi_handle_by_name("x", root));
   fail_if(handle_x == NULL);
   vhpi_printf("x handle %p", handle_x);

   vhpiPhysT x_val = VHPI_CHECK(vhpi_get_phys(vhpiPhysValP, handle_x));
   fail_unless(phys_to_i64(x_val) == 2);

   vhpiHandleT handle_weight_type = VHPI_CHECK(vhpi_handle(vhpiType, handle_x));
   fail_if(handle_weight_type == NULL);

   const char *weight_name = (char *)vhpi_get_str(vhpiNameP, handle_weight_type);
   fail_if(strcmp("WEIGHT", weight_name));
   const char *weight_fullname = (char *)vhpi_get_str(vhpiFullNameP, handle_weight_type);
   fail_if(strcmp("@WORK:VHPI3-TEST:WEIGHT", weight_fullname));

   vhpiHandleT handle_weight_cons =
      VHPI_CHECK(vhpi_handle_by_index(vhpiConstraints, handle_weight_type, 0));
   fail_if(handle_weight_cons == NULL);

   vhpiHandleT handle_weight_cons_iter =
      VHPI_CHECK(vhpi_iterator(vhpiConstraints, handle_weight_type));
   vhpiHandleT next = VHPI_CHECK(vhpi_scan(handle_weight_cons_iter));
   fail_unless(vhpi_compare_handles(next, handle_weight_cons));
   VHPI_CHECK(vhpi_release_handle(next));
   fail_unless(vhpi_scan(handle_weight_cons_iter) == NULL);

   vhpiPhysT weight_left = VHPI_CHECK(vhpi_get_phys(vhpiPhysLeftBoundP,
                                                    handle_weight_cons));
   fail_unless(phys_to_i64(weight_left) == -100);

   vhpiPhysT weight_right = VHPI_CHECK(vhpi_get_phys(vhpiPhysRightBoundP,
                                                     handle_weight_cons));
   fail_unless(phys_to_i64(weight_right) == 4000);

   vhpi_release_handle(handle_weight_cons);
   vhpi_release_handle(handle_weight_type);
   vhpi_release_handle(handle_x);
   vhpi_release_handle(root);
}

void vhpi3_startup(void)
{
   vhpi_printf("hello, world!");

   vhpiCbDataT cb_data1 = {
      .reason    = vhpiCbStartOfSimulation,
      .cb_rtn    = start_of_sim,
      .user_data = NULL,
   };
   vhpi_register_cb(&cb_data1, 0);
   check_error();
}
