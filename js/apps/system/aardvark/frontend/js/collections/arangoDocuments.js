/*jslint indent: 2, nomen: true, maxlen: 100, white: true, plusplus: true */
/*global require, exports, window, Backbone, arangoDocumentModel, _, arangoHelper, $*/
(function() {
  "use strict";

  window.arangoDocuments = window.PaginatedCollection.extend({
    collectionID: 1,

    filters: [],

    sortAttribute: "_key",

    url: '/_api/documents',
    model: window.arangoDocumentModel,

    loadTotal: function() {
      var self = this;
      $.ajax({
        cache: false,
        type: "GET",
        url: "/_api/collection/" + this.collectionID + "/count",
        contentType: "application/json",
        processData: false,
        async: false,
        success: function(data) {
          self.setTotal(data.count);
        }
      });
    },

    setCollection: function(id) {
      this.resetFilter();
      this.collectionID = id;
      this.setPage(1);
      this.loadTotal();
    },

    setSort: function(key) {
      this.sortAttribute = key;
    },

    getSort: function() {
      return this.sortAttribute;
    },

    addFilter: function(attr, op, val) {
      this.filters.push({
        attr: attr,
        op: op,
        val: val
      });
    },

    setFiltersForQuery: function(bindVars) {
      if (this.filters.length === 0) {
        return "";
      }
      var query = " FILTER",
      parts = _.map(this.filters, function(f, i) {
        var res = " x.`";
        res += f.attr;
        res += "` ";
        res += f.op;
        res += " @param";
        res += i;
        bindVars["param" + i] = f.val;
        return res;
      });
      return query + parts.join(" &&");
    },

    setPagesize: function(size) {
      this.setPageSize(size);
    },

    resetFilter: function() {
      this.filters = [];
    },

    getDocuments: function (callback) {
      window.progressView.show("Fetching documents...");
      var self = this,
          query,
          bindVars,
          tmp,
          queryObj;
      bindVars = {
        "@collection": this.collectionID,
        "offset": this.getOffset(),
        "count": this.getPageSize()
      };

      query = "FOR x in @@collection";
      query += this.setFiltersForQuery(bindVars);
      // Sort result, only useful for a small number of docs
      if (this.getTotal() < 12000) {
        if (this.getSort() === '_key') {
          query += " SORT TO_NUMBER(x." + this.getSort() + ") == 0 ? x."
                + this.getSort() + " : TO_NUMBER(x." + this.getSort() + ")";
        }
        else {
          query += " SORT x." + this.getSort();
        }
      }

      if (bindVars.count !== 'all') {
        query += " LIMIT @offset, @count RETURN x";
      }
      else {
        tmp = {
          "@collection": this.collectionID
        };
        bindVars = tmp;
        query += " RETURN x";
      }

      queryObj = {
        query: query,
        bindVars: bindVars
      };
      if (this.getTotal() < 10000 || this.filters.length > 0) {
        queryObj.options = {
          fullCount: true
        };
      }

      $.ajax({
        cache: false,
        type: 'POST',
        async: true,
        url: '/_api/cursor',
        data: JSON.stringify(queryObj),
        contentType: "application/json",
        success: function(data) {
          self.clearDocuments();
          if (data.extra && data.extra.fullCount !== undefined) {
            self.setTotal(data.extra.fullCount);
          }
          if (self.getTotal() !== 0) {
            _.each(data.result, function(v) {
              self.add({
                "id": v._id,
                "rev": v._rev,
                "key": v._key,
                "content": v
              });
            });
          }
          callback();
          window.progressView.hide();
        },
        error: function(data) {
          window.progressView.hide();
          arangoHelper.arangoNotification("Document error", "Could not fetch requested documents.");
        }
      });
    },

    clearDocuments: function () {
      this.reset();
    },

    updloadDocuments : function (file) {
      var result;
      $.ajax({
        type: "POST",
        async: false,
        url:
        '/_api/import?type=auto&collection='+
        encodeURIComponent(this.collectionID)+
        '&createCollection=false',
        data: file,
        processData: false,
        contentType: 'json',
        dataType: 'json',
        complete: function(xhr) {
          if (xhr.readyState === 4 && xhr.status === 201) {
            result =  true;
          } else {
            result =  "Upload error";
          }
        }
      });
      return result;
    }
  });
}());
