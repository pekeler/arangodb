/*global require, applicationContext*/

////////////////////////////////////////////////////////////////////////////////
/// @brief A Foxx.Controller to generate new FoxxApps
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2014 triagens GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Michael Hackstein
/// @author Copyright 2011-2013, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

"use strict";

var FoxxController = require("org/arangodb/foxx").Controller,
  controller = new FoxxController(applicationContext),
  internal = require("internal"),
  TemplateEngine = require("lib/foxxTemplateEngine").Engine,
  isDevMode = function() {
    return internal.developmentMode;
  };

controller.get("/devMode", function(req, res) {
  res.json(isDevMode());
});

controller.post("/generate", function(req, res) {
  var path,
    templateEngine;

  if (isDevMode()) {
    path = module.devAppPath();
  } else {
    path = module.appPath();
  }

  templateEngine = new TemplateEngine({
    applicationContext: applicationContext,
    path: path,
    name: 'fancyApp',
    collectionNames: ['people'],
    authenticated: false,
    author: 'Bob the Builder',
    description: 'This app is pretty fancy',
    license: 'Do Whatever You Want'
  });

  templateEngine.write();
});
