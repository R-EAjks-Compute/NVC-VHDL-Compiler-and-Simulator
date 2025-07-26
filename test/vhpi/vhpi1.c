#include "vhpi_test.h"

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static vhpiHandleT handle_x;
static vhpiHandleT handle_y;
static vhpiHandleT handle_sos;

static void test_bin_str(void)
{
   vhpiHandleT root = vhpi_handle(vhpiRootInst, NULL);
   check_error();

   vhpiHandleT hb = vhpi_handle_by_name("b", root);
   check_error();

   vhpiHandleT hv = vhpi_handle_by_name("v", root);
   check_error();

   vhpiHandleT hi = vhpi_handle_by_name("i", root);
   check_error();

   char b_str[2] = { 0xff, 0xff };
   vhpiValueT b_value = {
      .format    = vhpiBinStrVal,
      .bufSize   = sizeof(b_str),
      .value.str = (vhpiCharT *)b_str
   };
   vhpi_get_value(hb, &b_value);
   check_error();

   vhpi_printf("b bit string '%s' %x", b_str);
   fail_unless(strcmp(b_str, "0") == 0);

   b_value.format = vhpiLogicVal;
   vhpi_get_value(hb, &b_value);
   check_error();

   vhpi_printf("b logic %x", b_value.value.smallenumv);
   fail_unless(b_value.value.smallenumv == 0);

   b_value.format = vhpiIntVal;
   vhpi_get_value(hb, &b_value);
   check_error();

   vhpi_printf("b integer %x", b_value.value.intg);
   fail_unless(b_value.value.intg == 0);

   vhpiValueT i_value = {
      .format    = vhpiIntVal,
      .bufSize   = 0,
      .value.str = NULL
   };
   vhpi_get_value(hi, &i_value);
   check_error();

   vhpi_printf("i integer %x", i_value.value.intg);
   fail_unless(i_value.value.intg == 42);

   i_value.format = vhpiLongIntVal;
   i_value.value.longintg = 0xdead;
   vhpi_get_value(hi, &i_value);
   check_error();

   vhpi_printf("i long integer %lx", i_value.value.longintg);
   fail_unless(i_value.value.longintg == 42);

   i_value.format = vhpiSmallEnumVal;
   i_value.value.smallenumv = 0xde;
   int result = vhpi_get_value(hi, &i_value);
   fail_unless(result == -1);

   vhpiErrorInfoT errorinfo;
   fail_unless(vhpi_check_error(&errorinfo));
   fail_unless(errorinfo.severity == vhpiError);

   vhpiValueT v_value = {
      .format    = vhpiBinStrVal,
      .bufSize   = 0,
      .value.str = NULL
   };
   int need = vhpi_get_value(hv, &v_value);
   check_error();

   vhpi_printf("need %d bytes for v string", need);

   v_value.value.str = malloc(need);
   v_value.bufSize   = need;
   fail_if(v_value.value.str == NULL);
   fail_unless(vhpi_get_value(hv, &v_value) == 0);
   check_error();

   vhpi_printf("v bit string '%s'", v_value.value.str);
   fail_unless(strcmp((char *)v_value.value.str, "0011") == 0);
   fail_unless(v_value.numElems == 4);
   free(v_value.value.str);

   v_value.bufSize = 0;
   v_value.value.enumvs = NULL;
   v_value.format = vhpiLogicVecVal;

   need = vhpi_get_value(hv, &v_value);
   check_error();

   fail_unless(need = 4 * sizeof(vhpiEnumT));

   v_value.bufSize = 4 * sizeof(vhpiEnumT);
   v_value.value.enumvs = malloc(v_value.bufSize);
   fail_unless(vhpi_get_value(hv, &v_value) == 0);

   fail_unless(v_value.numElems == 4);

   fail_unless(v_value.value.enumvs[0] == 0);
   fail_unless(v_value.value.enumvs[1] == 0);
   fail_unless(v_value.value.enumvs[2] == 1);
   fail_unless(v_value.value.enumvs[3] == 1);

   free(v_value.value.enumvs);

   vhpi_release_handle(root);
   vhpi_release_handle(hb);
   vhpi_release_handle(hv);
}

static void y_value_change(const vhpiCbDataT *cb_data)
{
   vhpiTimeT now;
   vhpi_get_time(&now, NULL);

   fail_unless(now.low == cb_data->time->low);
   fail_unless(now.high == cb_data->time->high);

   vhpiValueT value = {
      .format = vhpiObjTypeVal
   };
   vhpi_get_value(handle_y, &value);
   check_error();
   fail_unless(value.format == vhpiIntVal);
   fail_unless(value.numElems == 1);

   vhpi_printf("y value changed to %d", value.value.intg);

   if (value.value.intg == 75) {
      test_bin_str();

      vhpi_put_value(handle_x, &value, vhpiRelease);
      check_error();

      vhpi_control(vhpiFinish);
      check_error();
   }
   else {
      value.value.intg++;
      vhpi_put_value(handle_x, &value, vhpiForcePropagate);
      check_error();
   }
}

static vhpiHandleT defer_disable;
static vhpiHandleT defer_enable;

static void deferred_work(const vhpiCbDataT *cb_data)
{
   vhpi_printf("deferred work callback!");

   vhpi_disable_cb(defer_disable);
   vhpi_enable_cb(defer_enable);
}

static vhpiHandleT mutual_cb1[2], mutual_cb2[2], mutual_cb3[2];

static void mutual(const vhpiCbDataT *cb_data)
{
   vhpi_printf("mutual callback!");

   vhpiHandleT *cb = cb_data->user_data;
   vhpi_remove_cb(cb[0]);
   vhpi_remove_cb(cb[1]);
}

static void disabled_callback(const vhpiCbDataT *cb_data)
{
   vhpi_printf("disabled callback!");
   fail_if(1);
}

static void enabled_callback(const vhpiCbDataT *cb_data)
{
   vhpi_printf("enabled callback!");

   vhpiTimeT now;
   vhpi_get_time(&now, NULL);

   fail_unless(now.low == 5000002);
   fail_unless(now.high == 0);
}

static void after_5ns(const vhpiCbDataT *cb_data)
{
   vhpi_printf("after_5ns callback!");

   long cycles;
   vhpiTimeT now;
   vhpi_get_time(&now, &cycles);

   fail_unless(now.low == 5000000);
   fail_unless(now.high == 0);
   fail_unless(cycles == 0);

   vhpiValueT value = {
      .format = vhpiObjTypeVal
   };
   vhpi_get_value(handle_y, &value);
   check_error();
   fail_unless(value.format == vhpiIntVal);
   vhpi_printf("value=%d", value.value.intg);
   fail_unless(value.value.intg == 6);

   value.value.intg = 70;
   vhpi_put_value(handle_x, &value, vhpiForcePropagate);
   check_error();

   vhpiCbDataT cb_data2 = {
      .reason = vhpiCbValueChange,
      .cb_rtn = y_value_change,
      .obj    = handle_y,
      .time   = (vhpiTimeT *)-1
   };
   vhpi_register_cb(&cb_data2, 0);
   check_error();

   cb_data2.cb_rtn = disabled_callback;
   vhpiHandleT cb = vhpi_register_cb(&cb_data2, vhpiReturnCb);
   check_error();
   fail_if(vhpi_disable_cb(cb));

   cb_data2.cb_rtn = mutual;
   cb_data2.user_data = mutual_cb1;
   mutual_cb1[0] = vhpi_register_cb(&cb_data2, vhpiReturnCb);
   check_error();
   mutual_cb1[1] = vhpi_register_cb(&cb_data2, vhpiReturnCb);
   check_error();

   vhpiTimeT time = {
      .low = 1
   };

   vhpiCbDataT cb_data3 = {
      .reason = vhpiCbAfterDelay,
      .cb_rtn = disabled_callback,
      .time   = &time
   };
   cb = vhpi_register_cb(&cb_data3, vhpiReturnCb);
   check_error();
   fail_if(vhpi_disable_cb(cb));

   cb = vhpi_register_cb(&cb_data3, vhpiReturnCb);
   check_error();
   fail_if(vhpi_remove_cb(cb));

   cb = vhpi_register_cb(&cb_data3, vhpiReturnCb);
   check_error();
   fail_if(vhpi_disable_cb(cb));
   fail_if(vhpi_remove_cb(cb));

   vhpi_register_cb(&cb_data3, vhpiDisableCb);
   check_error();

   time.low = 2;
   defer_disable = vhpi_register_cb(&cb_data3, vhpiReturnCb);
   check_error();

   cb_data3.cb_rtn = enabled_callback;
   defer_enable = vhpi_register_cb(&cb_data3, vhpiReturnCb | vhpiDisableCb);
   check_error();

   vhpiCbDataT cb_data4 = {
      .reason    = vhpiCbAfterDelay,
      .cb_rtn    = deferred_work,
      .time      = &time,
   };

   time.low = 1;
   vhpi_register_cb(&cb_data4, 0);
   check_error();

   cb_data4.cb_rtn = mutual;
   cb_data4.user_data = mutual_cb2;
   mutual_cb2[0] = vhpi_register_cb(&cb_data4, vhpiReturnCb);
   check_error();
   mutual_cb2[1] = vhpi_register_cb(&cb_data4, vhpiReturnCb);
   check_error();

   vhpiCbDataT cb_data5 = {
      .reason = vhpiCbNextTimeStep,
      .cb_rtn = disabled_callback,
   };
   cb = vhpi_register_cb(&cb_data5, vhpiReturnCb);
   check_error();
   fail_if(vhpi_remove_cb(cb));
   vhpi_release_handle(cb);

   cb_data5.cb_rtn = mutual;
   cb_data5.user_data = mutual_cb3;
   mutual_cb3[0] = vhpi_register_cb(&cb_data5, vhpiReturnCb);
   check_error();
   mutual_cb3[1] = vhpi_register_cb(&cb_data5, vhpiReturnCb);
   check_error();
}

static void start_of_sim(const vhpiCbDataT *cb_data)
{
   vhpi_printf("start of sim callback! user data is '%s'",
               (char *)cb_data->user_data);

   long cycles;
   vhpiTimeT now;
   vhpi_get_time(&now, &cycles);

   fail_unless(now.low == 0);
   fail_unless(now.high == 0);
   fail_unless(cycles == 0);

   vhpiValueT value = {
      .format = vhpiObjTypeVal
   };
   vhpi_get_value(handle_x, &value);
   check_error();
   fail_unless(value.format == vhpiIntVal);
   fail_unless(value.value.intg == 0);

   value.value.intg = 5;
   vhpi_put_value(handle_x, &value, vhpiForcePropagate);
   check_error();

   vhpiTimeT time_5ns = {
      .low = 5000000
   };

   vhpiCbDataT cb_data2 = {
      .reason = vhpiCbAfterDelay,
      .cb_rtn = after_5ns,
      .time   = &time_5ns
   };
   vhpi_register_cb(&cb_data2, 0);
   check_error();

   vhpiTimeT next;
   fail_unless(vhpi_get_next_time(&next) == 0);
   fail_unless(next.high == 0);
   fail_unless(next.low == 5000000);
}

static void end_of_sim(const vhpiCbDataT *cb_data)
{
   vhpi_printf("end of sim callback");

   vhpiValueT value = {
      .format = vhpiObjTypeVal
   };
   vhpi_get_value(handle_y, &value);
   check_error();
   fail_unless(value.format == vhpiIntVal);
   fail_unless(value.value.intg == 75);

   vhpi_release_handle(handle_x);
   vhpi_release_handle(handle_y);
   vhpi_release_handle(handle_sos);
}

static void end_of_init(const vhpiCbDataT *cb_data)
{
   vhpi_printf("end of init callback");

   vhpiHandleT tool = VHPI_CHECK(vhpi_handle(vhpiTool, NULL));
   fail_if(tool == NULL);
   vhpi_printf("tool is %s", vhpi_get_str(vhpiNameP, tool));
   vhpi_printf("tool version is %s", vhpi_get_str(vhpiToolVersionP, tool));

   fail_unless(cb_data->time == NULL);
   fail_unless(cb_data->value == NULL);
   fail_unless(cb_data->obj == NULL);

   vhpiHandleT args = vhpi_iterator(vhpiArgvs, tool);
   fail_if(args == NULL);
   int i = 0;
   for (vhpiHandleT arg = vhpi_scan(args); arg != NULL; arg = vhpi_scan(args), i++)
      vhpi_printf("arg is %s", vhpi_get_str(vhpiStrValP, arg));
   fail_unless(vhpi_get(vhpiArgcP, tool) == i);

   vhpi_release_handle(tool);

   vhpiHandleT root = VHPI_CHECK(vhpi_handle(vhpiRootInst, NULL));
   fail_if(root == NULL);
   vhpi_printf("root handle %p", root);

   const vhpiCharT *root_name = vhpi_get_str(vhpiNameP, root);
   vhpi_printf("root name is %s", root_name);
   vhpi_printf("root case name is %s", vhpi_get_str(vhpiCaseNameP, root));
   vhpi_printf("root full name is %s", vhpi_get_str(vhpiFullNameP, root));
   vhpi_printf("root full case name is %s", vhpi_get_str(vhpiFullCaseNameP, root));

   fail_unless(vhpi_scan(root) == NULL);

   vhpiHandleT root_ports = vhpi_iterator(vhpiPortDecls, root);
   fail_if(root_ports == NULL);
   i = 0;
   for (vhpiHandleT port = vhpi_scan(root_ports); port != NULL; port = vhpi_scan(root_ports), i++) {
      vhpi_printf("root port is %s", vhpi_get_str(vhpiNameP, port));
      vhpiHandleT by_index = vhpi_handle_by_index(vhpiPortDecls, root, i);
      fail_unless(vhpi_compare_handles(port, by_index));
   }

   vhpiHandleT root_signals = vhpi_iterator(vhpiSigDecls, root);
   fail_if(root_signals == NULL);
   i = 0;
   for (vhpiHandleT decl = vhpi_scan(root_signals); decl != NULL; decl = vhpi_scan(root_signals))
      vhpi_printf("root signal is %s", vhpi_get_str(vhpiNameP, decl));

   vhpiHandleT root_decls = vhpi_iterator(vhpiDecls, root);
   fail_if(root_decls == NULL);
   i = 0;
   for (vhpiHandleT decl = vhpi_scan(root_decls); decl != NULL; decl = vhpi_scan(root_decls), i++) {
      vhpi_printf("root decl is %s", vhpi_get_str(vhpiNameP, decl));
      vhpiHandleT by_index = vhpi_handle_by_index(vhpiDecls, root, i);
      fail_unless(vhpi_compare_handles(decl, by_index));
   }

   vhpiHandleT arch = VHPI_CHECK(vhpi_handle(vhpiDesignUnit, root));
   fail_if(arch == NULL);
   vhpi_printf("arch handle %p", arch);

   vhpi_printf("arch name is %s", vhpi_get_str(vhpiNameP, arch));
   vhpi_printf("arch unit name is %s", vhpi_get_str(vhpiUnitNameP, arch));

   vhpiHandleT entity = VHPI_CHECK(vhpi_handle(vhpiPrimaryUnit, arch));
   fail_if(entity == NULL);
   vhpi_printf("entity handle %p", entity);

   vhpi_printf("entity name is %s", vhpi_get_str(vhpiNameP, entity));
   vhpi_printf("entity unit name is %s", vhpi_get_str(vhpiUnitNameP, entity));
   vhpi_release_handle(entity);
   vhpi_release_handle(arch);

   handle_x = VHPI_CHECK(vhpi_handle_by_name("x", root));
   fail_if(handle_x == NULL);
   vhpi_printf("x handle %p", handle_x);
   vhpi_printf("x name %s", vhpi_get_str(vhpiNameP, handle_x));
   vhpi_printf("x full name is %s", vhpi_get_str(vhpiFullNameP, handle_x));

   handle_y = VHPI_CHECK(vhpi_handle_by_name("y", root));
   fail_if(handle_y == NULL);
   vhpi_printf("y handle %p", handle_y);
   vhpi_printf("y name %s", vhpi_get_str(vhpiNameP, handle_y));
   vhpi_printf("y full name is %s", vhpi_get_str(vhpiFullNameP, handle_y));

   vhpiHandleT handle_y2 = VHPI_CHECK(vhpi_handle_by_name(":vhpi1.y", NULL));
   fail_unless(vhpi_compare_handles(handle_y, handle_y2));
   vhpi_release_handle(handle_y2);

   vhpiHandleT handle_y3 = VHPI_CHECK(vhpi_handle_by_name("vhpi1.y", NULL));
   fail_unless(vhpi_compare_handles(handle_y, handle_y3));
   vhpi_release_handle(handle_y3);

   fail_unless(vhpi_get(vhpiKindP, handle_x) == vhpiPortDeclK);
   fail_unless(vhpi_get(vhpiModeP, handle_x) == vhpiInMode);
   fail_unless(vhpi_get(vhpiModeP, handle_y) == vhpiOutMode);

   fail_unless(vhpi_get(vhpiIsLocalP, handle_x) == vhpiUndefined);

   vhpiErrorInfoT info;
   fail_unless(vhpi_check_error(&info));

   vhpiHandleT handle_r = VHPI_CHECK(vhpi_handle_by_name("r", root));
   fail_if(handle_r == NULL);
   vhpi_printf("r handle %p", handle_r);

   vhpiHandleT r_type = VHPI_CHECK(vhpi_handle(vhpiType, handle_r));
   fail_if(r_type == NULL);
   vhpi_printf("r type handle %p", r_type);
   vhpi_printf("r type name is %s", vhpi_get_str(vhpiNameP, r_type));
   vhpi_printf("r type full name is %s", vhpi_get_str(vhpiFullNameP, r_type));

   vhpiHandleT r_constrs = VHPI_CHECK(vhpi_iterator(vhpiConstraints, r_type));
   fail_if(r_constrs == NULL);

   vhpiHandleT r_range = VHPI_CHECK(vhpi_scan(r_constrs));
   fail_if(r_range == NULL);
   fail_unless(vhpi_scan(r_constrs) == NULL);
   vhpi_printf("r type range handle %p", r_range);
   vhpi_printf("r left bound %f", vhpi_get_real(vhpiFloatLeftBoundP, r_range));
   vhpi_printf("r right bound %f", vhpi_get_real(vhpiFloatRightBoundP, r_range));

   fail_unless(vhpi_get_real(vhpiFloatLeftBoundP, r_range) == 0.0);
   fail_unless(vhpi_get_real(vhpiFloatRightBoundP, r_range) == 1.0);

   vhpi_release_handle(r_range);
   vhpi_release_handle(r_constrs);
   vhpi_release_handle(r_type);

   vhpiHandleT handle_v = VHPI_CHECK(vhpi_handle_by_name("v", root));
   fail_if(handle_v == NULL);
   vhpi_printf("v handle %p", handle_v);
   vhpi_printf("v name is %s", vhpi_get_str(vhpiNameP, handle_v));
   vhpi_printf("v full name is %s", vhpi_get_str(vhpiFullNameP, handle_v));
   fail_unless(vhpi_get(vhpiStaticnessP, handle_v) == vhpiDynamic);

   vhpiHandleT v_type = VHPI_CHECK(vhpi_handle(vhpiType, handle_v));
   fail_if(v_type == NULL);
   vhpi_printf("v type handle %p", v_type);
   vhpi_printf("v type name is %s", vhpi_get_str(vhpiNameP, v_type));
   vhpi_printf("v type full name is %s", vhpi_get_str(vhpiFullNameP, v_type));
   vhpi_printf("v dimensions %d", vhpi_get(vhpiNumDimensionsP, v_type));
   fail_if(vhpi_get(vhpiIsUnconstrainedP, v_type));

   vhpiHandleT v_constrs = VHPI_CHECK(vhpi_iterator(vhpiConstraints, v_type));
   fail_if(v_constrs == NULL);

   vhpiHandleT v_range = VHPI_CHECK(vhpi_scan(v_constrs));
   fail_if(v_range == NULL);
   fail_unless(vhpi_scan(v_constrs) == NULL);
   vhpi_printf("v type range handle %p", v_range);
   vhpi_printf("v left bound %d", vhpi_get(vhpiLeftBoundP, v_range));
   vhpi_printf("v right bound %d", vhpi_get(vhpiRightBoundP, v_range));

   vhpiHandleT v_elem = VHPI_CHECK(vhpi_handle(vhpiElemType, v_type));
   vhpi_printf("v elem type handle %p", v_elem);
   vhpi_printf("v elem type name is %s", vhpi_get_str(vhpiNameP, v_elem));
   vhpi_printf("v elem type full name is %s", vhpi_get_str(vhpiFullNameP, v_elem));

   vhpiIntT nlits = VHPI_CHECK(vhpi_get(vhpiNumLiteralsP, v_elem));
   vhpiHandleT v_lits = vhpi_iterator(vhpiEnumLiterals, v_elem);
   i = 0;
   for (vhpiHandleT lit = vhpi_scan(v_lits);
        lit != NULL;
        lit = vhpi_scan(v_lits), i++) {
      vhpi_printf("v elem literal %d is %s", i, vhpi_get_str(vhpiStrValP, lit));
      fail_unless(vhpi_get(vhpiPositionP, lit) == i);
   }
   fail_unless(i == nlits);

   vhpiHandleT v_names = vhpi_iterator(vhpiIndexedNames, handle_v);
   fail_if(v_names == NULL);
   for (vhpiHandleT name = vhpi_scan(v_names); name != NULL; name = vhpi_scan(v_names))
      vhpi_printf("v indexed name is %s", vhpi_get_str(vhpiNameP, name));

   vhpiHandleT handle_case =
      VHPI_CHECK(vhpi_handle_by_name("a_name_with_mixed_case", root));
   fail_if(handle_case == NULL);
   vhpi_printf("handle %p", handle_case);
   vhpi_printf("name is %s", vhpi_get_str(vhpiNameP, handle_case));
   vhpi_printf("case name is %s", vhpi_get_str(vhpiCaseNameP, handle_case));
   vhpi_printf("full case name is %s", vhpi_get_str(vhpiFullCaseNameP, handle_case));

   vhpiHandleT handle_error = vhpi_handle_by_name("sfdf", root);
   fail_unless(vhpi_check_error(&info));
   fail_unless(handle_error == NULL);

   vhpi_release_handle(root);
   check_error();

   // Releasing the handle twice should report an error
   vhpi_release_handle(root);
   fail_unless(vhpi_check_error(&info));
}

void vhpi1_startup(void)
{
   vhpi_printf("hello, world!");

   vhpiCbDataT cb_data1 = {
      .reason    = vhpiCbStartOfSimulation,
      .cb_rtn    = start_of_sim,
      .user_data = (char *)"some user data",
   };
   handle_sos = VHPI_CHECK(vhpi_register_cb(&cb_data1, vhpiReturnCb));
   fail_unless(vhpi_get(vhpiStateP, handle_sos) == vhpiEnable);

   vhpiCbDataT cb_data_rtn;
   vhpi_get_cb_info(handle_sos, &cb_data_rtn);
   fail_unless(cb_data_rtn.reason == vhpiCbStartOfSimulation);
   fail_unless(cb_data_rtn.cb_rtn == start_of_sim);
   fail_unless(cb_data_rtn.user_data == cb_data1.user_data);

   vhpiCbDataT cb_data2 = {
      .reason = vhpiCbEndOfSimulation,
      .cb_rtn = end_of_sim
   };
   VHPI_CHECK(vhpi_register_cb(&cb_data2, 0));

   vhpiCbDataT cb_data3 = {
      .reason = vhpiCbEndOfInitialization,
      .cb_rtn = end_of_init,
      .obj    = (vhpiHandleT)0x123456,
      .time   = (vhpiTimeT *)0xdeadbeef,
      .value  = (vhpiValueT *)0x5add00d,
   };
   vhpi_register_cb(&cb_data3, 0);

   vhpiErrorInfoT info;
   fail_unless(vhpi_check_error(&info));
   fail_unless(info.severity == vhpiWarning);
}
