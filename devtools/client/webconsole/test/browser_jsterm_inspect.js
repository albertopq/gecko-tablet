/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set ft=javascript ts=2 et sw=2 tw=80: */
/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

// Check that the inspect() jsterm helper function works.

"use strict";

const TEST_URI = "data:text/html;charset=utf8,<p>hello bug 869981";

add_task(function* () {
  yield loadTab(TEST_URI);

  let hud = yield openConsole();
  let jsterm = hud.jsterm;

  jsterm.execute("testProp = 'testValue'");

  let fetched = jsterm.once("variablesview-fetched");
  jsterm.execute("inspect(window)");
  let variable = yield fetched;

  ok(variable._variablesView, "variables view object");

  yield findVariableViewProperties(variable, [
    { name: "testProp", value: "testValue" },
    { name: "document", value: /HTMLDocument \u2192 data:/ },
  ], { webconsole: hud });
});
