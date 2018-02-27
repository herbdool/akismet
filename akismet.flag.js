(function ($) {

/**
 * Only show links to report comments after the comment has been moused over.
 */
Drupal.behaviors.akismetReportComment = {
  showReportLink: function(e) {
    $(this).find('.akismet-flag').show();
  },

  hideReportLink: function(e) {
    if ($(this).find('.akismet-flag-reasons').length === 0) {
      // Only hide the link if the comment is not currently being reported.
      $(this).find('.akismet-flag').hide();
    }
  },

  attach: function (context) {
    // Leave the report links visible for touch devices.
    if (!!('ontouchstart' in window) || !!('msmaxtouchpoints' in window.navigator)) {
      return;
    }

    // Don't show/hide the report link if the text is aligned right or its 
    // appearance will cause all other inline links to jump to the left.
    var ul = $(context).find('.comment ul.links:has(.akismet-flag)');
    if ((ul.css('display') == 'block' && ul.css('textAlign') == 'right')
        || ul.css('float') == 'right'
        || ul.find('li').css('float') == 'right') {
    } else {
      $(context).find('.comment:has(.akismet-flag)').bind('mouseover',this.showReportLink);
      $(context).find('.comment:has(.akismet-flag)').bind('mouseout',this.hideReportLink);
      $(context).find('.comment .akismet-flag').hide();
    }
  },

  detach: function(context) {
    $(context).find('.comment:has(.akismet-flag)').unbind('mouseover',this.showReportLink);
    $(context).find('.comment:has(.akismet-flag)').unbind('mouseout',this.hideReportLink);
  }
};

/**
 * Close a form to report comments as inappropriate if user clicks outside.
 */
Drupal.behaviors.akismetReportCancel = {
  lastFocus: null,
  context: null,

  // Helper functions have "this" set to Drupal.behaviors.akismetReportCancel.
  closeReportDialog: function(e) {
    if ($('.akismet-flag-container').length > 0) {
      e.stopPropagation();
      $('.akismet-flag-container').remove();
      this.lastFocus.focus();
    }
  },

  checkEscapeDialog: function(e) {
    if (e.keyCode === 27) {
      this.closeReportDialog(e);
    }

  },

  checkClickOutside: function(e) {
    if (!$.contains(this.context[0],e.target)) {
      this.closeReportDialog(e);
    }
  },

  attach: function (context) {
    if ($(context).hasClass('akismet-flag-container')) {
      // Enable and set focus on the new dialog.
      this.context = context;
      this.lastFocus = document.activeElement;
      context.tabIndex = -1;
      context.focus();

      $(document).bind('keydown',$.proxy(this.checkEscapeDialog,this));
      $(document).bind('click',$.proxy(this.checkClickOutside,this));
    }
  },
  detach: function(context) {
    if ($(context).hasClass('akismet-flag-container')) {
      $(document).unbind('keydown',this.checkCloseDialog);
      $(document).unbind('click',this.checkClickOutside);
    }
  }
};

/**
 * Add a class to reported content to allow overriding the display with CSS.
 */
Drupal.behaviors.akismetReportedMarkAsFlagged = {
  attach: function (context) {
    if ($(context).hasClass('akismet-flag-confirm')) {
      $(context).parents('.akismet-flag-content').addClass('akismet-flagged');
    }
  }
}

/**
 * Close reporting confirmation and remove a comment that has been reported.
 */
Drupal.behaviors.akismetReportedCommentHide = {
  attach: function (context) {
    if ($(context).hasClass('akismet-flag-confirm')) {
      // Removes the comment from view.
      $(context).parent().delay(1000).fadeOut(400,function() {
        $(context).parents('.akismet-flag-content-comment').prev().remove();
        $(context).parents('.akismet-flag-content-comment').slideUp(200, this.remove);
      });
    }
  }
};

})(jQuery);
