(function ($) {

Drupal.akismet = Drupal.akismet || {};

/**
 * Open links to Akismet.com in a new window.
 *
 * Required for valid XHTML Strict markup.
 */
Drupal.behaviors.akismetTarget = {
  attach: function (context) {
    $(context).find('.akismet-target').click(function () {
      this.target = '_blank';
    });
  }
};

/**
 * Retrieve and attach the form behavior analysis tracking image if it has not
 * yet been added for the form.
 */
Drupal.behaviors.akismetFBA = {
  attach: function (context, settings) {
    $(':input[name="akismet[fba]"][value=""]', context).once().each(function() {
      $input = $(this);
      $.ajax({
        url: Drupal.settings.basePath + Drupal.settings.pathPrefix + 'akismet/fba',
        type: 'POST',
        dataType: 'json',
        success: function(data) {
          if (!data.tracking_id || !data.tracking_url) {
            return;
          }
          // Save the tracking id in the hidden field.
          $input.val(data.tracking_id);
          // Attach the tracking image.
          $('<img src="' + data.tracking_url + '" width="1" height="1" alt="" />').appendTo('body');
        }
      })
    });
  }
};

 /**
 * Attach click event handlers for CAPTCHA links.
 */
Drupal.behaviors.akismetCaptcha = {
  attach: function (context, settings) {
    $('a.akismet-switch-captcha', context).click(function (e) {
      var $akismetForm = $(this).parents('form');
      var newCaptchaType = $(this).hasClass('akismet-audio-captcha') ? 'audio' : 'image';
      Drupal.akismet.getAkismetCaptcha(newCaptchaType, $akismetForm);
    });
    $('a.akismet-refresh-captcha', context).click(function (e) {
      var $akismetForm = $(this).parents('form');
      var currentCaptchaType = $(this).hasClass('akismet-refresh-audio') ? 'audio' : 'image';
      Drupal.akismet.getAkismetCaptcha(currentCaptchaType, $akismetForm);
    });
  }
};

/**
 * Fetch a Akismet CAPTCHA and output the image or audio into the form.
 *
 * @param captchaType
 *   The type of CAPTCHA to retrieve; one of "audio" or "image".
 * @param context
 *   The form context for this retrieval.
 */
Drupal.akismet.getAkismetCaptcha = function (captchaType, context) {
  var formBuildId = $('input[name="form_build_id"]', context).val();
  var akismetContentId = $('input.akismet-content-id', context).val();

  var path = 'akismet/captcha/' + captchaType + '/' + formBuildId;
  if (akismetContentId) {
    path += '/' + akismetContentId;
  }
  path += '?cb=' + new Date().getTime();

  // Retrieve a new CAPTCHA.
  $.ajax({
    url: Drupal.settings.basePath + Drupal.settings.pathPrefix + path,
    type: 'POST',
    dataType: 'json',
    success: function (data) {
      if (!(data && data.content)) {
        return;
      }
      // Inject new CAPTCHA.
      $('.akismet-captcha-content', context).parent().html(data.content);
      // Update CAPTCHA ID.
      $('input.akismet-captcha-id', context).val(data.captcha_id);
      // Add an onclick-event handler for the new link.
      Drupal.attachBehaviors(context);
      // Focus on the CAPTCHA input.
      if (captchaType == 'image') {
          $('input[name="akismet[captcha]"]', context).focus();
      } else {
         // Focus on audio player.
         // Fallback player code is responsible for setting focus upon embed.
         if ($('#akismet_captcha_audio').is(":visible")) {
             $('#akismet_captcha_audio').focus();
         }
      }
    }
  });
  return false;
}

})(jQuery);
