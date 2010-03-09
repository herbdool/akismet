// $Id$

(function ($) {

/**
 * Open Mollom privacy policy link in a new window.
 *
 * Required for valid XHTML Strict markup.
 */
Drupal.behaviors.mollomPrivacy = {
  attach: function (context) {
    $('.mollom-privacy a', context).click(function () {
      this.target = '_blank';
    });
  }
};

/**
 * Attach click event handlers for CAPTCHA links.
 */
Drupal.behaviors.mollomCaptcha = {
  attach: function (context, settings) {
    // @todo Pass the local settings we get from Drupal.attachBehaviors(), or
    //   inline the click event handlers, or turn them into methods of this
    //   behavior object.
    $('a.mollom-audio-captcha', context).click(getAudioCaptcha);
    $('a.mollom-image-captcha', context).click(getImageCaptcha);
  }
};

function getAudioCaptcha() {
  var context = $(this).parents('form');

  // Extract the Mollom session id and form build id from the form.
  var mollomSessionId = $('input.mollom-session-id', context).val();
  var formBuildId = $('input[name="form_build_id"]', context).val();

  // Retrieve an audio CAPTCHA:
  $.getJSON(Drupal.settings.basePath + 'mollom/captcha/audio/' + formBuildId + '/' + mollomSessionId,
    function (data) {
      // Inject new audio CAPTCHA.
      $('.mollom-captcha-content', context).parent().html(data.content);
      // Update session id.
      $('input.mollom-session-id', context).val(data.session_id);
      // Add an onclick-event handler for the new link.
      $('a.mollom-image-captcha', context).click(getImageCaptcha);
    }
  );
  return false;
}

function getImageCaptcha() {
  var context = $(this).parents('form');

  // Extract the Mollom session id and form build id from the form.
  var mollomSessionId = $('input.mollom-session-id', context).val();
  var formBuildId = $('input[name="form_build_id"]', context).val();

  // Retrieve an image CAPTCHA:
  $.getJSON(Drupal.settings.basePath + 'mollom/captcha/image/' + formBuildId + '/' + mollomSessionId,
    function (data) {
      // Inject new image CAPTCHA.
      $('.mollom-captcha-content', context).parent().html(data.content);
      // Update session id.
      $('input.mollom-session-id', context).val(data.session_id);
      // Add an onclick-event handler for the new link.
      $('a.mollom-audio-captcha', context).click(getAudioCaptcha);
    }
  );
  return false;
}

})(jQuery);
