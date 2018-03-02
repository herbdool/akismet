<?php

/**
 * @file
 * Administrative page callbacks for Akismet module.
 */

/**
 * Helper function to prepare a list of available languages.
 */
function _akismet_supported_languages() {
  // D7-: _locale_prepare_predefined_list() removes installed languages, which
  // is the exact opposite of what we want.
  include_once DRUPAL_ROOT . '/includes/iso.inc';
  $predefined = _locale_get_predefined_list();
  $supported = array_flip(AKISMET::$LANGUAGES_SUPPORTED);
  $supported = array_combine(array_keys($supported), array_keys($supported));
  
  // Define those mappings that differ between Drupal codes and Akismet codes.
  $mapped = array(
    'nb' => 'no',
    'zh-hans' => 'zh-cn',
    'zh-hant' => 'zh-tw',
  );
  foreach($mapped as $drupal_key => $akismet_key) {
    if (isset($supported[$akismet_key])) {
      $supported[$drupal_key] = $akismet_key;
      unset($supported[$akismet_key]);
    }
  }

  $options = array();
  $languages_enabled = language_list();
  $installed_languages = array();

  // This does assume that all Akismet supported languages are in the predefined
  // Drupal list.
  foreach ($predefined as $langcode => $language) {
    $found = FALSE;
    $simplified_code = strtok($langcode, '-');
    if (isset($supported[$simplified_code]) && !isset($options[$simplified_code])) {
      $options[$supported[$simplified_code]] = t($language[0]);
      $found = TRUE;
    }
    else if (isset($supported[$langcode]) && !isset($options[$langcode])) {
      $options[$supported[$langcode]] = t($language[0]);
      $found = TRUE;
    }
    if ($found) {
      // Update the list of installed languages that are supported.
      if (isset($languages_enabled[$langcode])) {
        $installed_languages[$supported[$simplified_code]] = $simplified_code;
      }
    }
  }
  // Sort by translated option labels.
  asort($options);
  // UX: Sort installed languages first.
  return array_intersect_key($options, $installed_languages) + $options;
}

/**
 * Checks the configuration status on Akismet administration pages.
 *
 * On all Akismet administration pages, check the module configuration and
 * display the corresponding requirements error, if invalid.
 */
function akismet_admin_site_status($force = FALSE, $update = FALSE) {
  $status = _akismet_status($force, $update);
  if (empty($_POST) && !$status['isVerified']) {
    // Fetch and display requirements error message, without re-checking.
    module_load_install('akismet');
    $requirements = akismet_requirements('runtime', FALSE);
    if (isset($requirements['akismet']['description'])) {
      drupal_set_message($requirements['akismet']['description'], 'error');
    }
  }
  return $status;
}

/**
 * Menu callback; Displays a list of forms configured for Akismet.
 */
function akismet_admin_form_list() {
  akismet_admin_site_status();
  _akismet_testing_mode_warning();

  // Reset the cached list of protected forms.
  akismet_form_cache(TRUE);

  $modes = array(
    AKISMET_MODE_ANALYSIS => t('Text analysis'),
    AKISMET_MODE_CAPTCHA => t('CAPTCHA'),
  );

  $header = array(
    t('Form'),
    t('Protection mode'),
    array('data' => t('Operations'), 'colspan' => 2),
  );
  $result = db_query('SELECT form_id FROM {akismet_form}')->fetchCol();
  $forms = array();
  $module_info = system_get_info('module');
  foreach ($result as $form_id) {
    $forms[$form_id] = akismet_form_load($form_id);
    // system_get_info() only supports enabled modules. Default to the module's
    // machine name in case it is disabled.
    $module = $forms[$form_id]['module'];
    if (!isset($module_info[$module])) {
      $module_info[$module]['name'] = $module;
    }
    $forms[$form_id]['title'] = t('!module: !form-title', array(
      '!form-title' => $forms[$form_id]['title'],
      '!module' => t($module_info[$module]['name']),
    ));
  }

  // Sort forms by title (including module name prefix).
  uasort($forms, 'drupal_sort_title');

  $rows = array();
  foreach ($forms as $form_id => $akismet_form) {
    $row_attributes = array();
    $row = array();
    $row[] = $akismet_form['title'];
    if (isset($modes[$akismet_form['mode']])) {
      if ($akismet_form['mode'] == AKISMET_MODE_ANALYSIS) {
        // @todo Output unsure mode in summary listing.
        $row[] = t('!protection-mode (@discard)', array(
          '!protection-mode' => $modes[$akismet_form['mode']],
          '@discard' => $akismet_form['discard'] ? t('discard') : t('retain'),
        ));
      }
      else {
        $row[] = $modes[$akismet_form['mode']];
      }
    }
    else {
      $row[] = t('- orphan -');
    }
    if (empty($akismet_form['orphan'])) {
      $row[] = array('data' => array(
        '#type' => 'link',
        '#title' => t('Configure'),
        '#href' => 'admin/config/content/akismet/manage/' . $form_id,
      ));
    }
    else {
      $row[] = '';
      $row_attributes['class'] = array('error');
      drupal_set_message(t("%module module's %form_id form no longer exists.", array(
        '%form_id' => $form_id,
        '%module' => isset($module_info[$akismet_form['module']]['name']) ? t($module_info[$akismet_form['module']]['name']) : $akismet_form['module'],
      )), 'warning');
    }
    $row[] = array('data' => array(
      '#type' => 'link',
      '#title' => t('Unprotect'),
      '#href' => 'admin/config/content/akismet/unprotect/' . $form_id,
    ));

    $rows[] = $row_attributes + array('data' => $row);
  }

  $build['forms'] = array(
    '#theme' => 'table',
    '#header' => $header,
    '#rows' => $rows,
    '#empty' => l(t('Add form'), 'admin/config/content/akismet/add'),
  );

  return $build;
}

/**
 * Return registered forms as an array suitable for a 'checkboxes' form element #options property.
 */
function akismet_admin_form_options() {
  // Retrieve all registered forms.
  $form_list = akismet_form_list();

  // Remove already configured form ids.
  $result = db_query('SELECT form_id FROM {akismet_form}')->fetchCol();
  foreach ($result as $form_id) {
    unset($form_list[$form_id]);
  }
  // If all registered forms are configured already, output a message, and
  // redirect the user back to overview.
  if (empty($form_list)) {
    drupal_set_message(t('All available forms are protected already.'));
    drupal_goto('admin/config/content/akismet');
  }

  // Load module information.
  $module_info = system_get_info('module');

  // Transform form information into an associative array suitable for #options.
  $options = array();
  foreach ($form_list as $form_id => $info) {
    // system_get_info() only supports enabled modules. Default to the module's
    // machine name in case it is disabled.
    $module = $info['module'];
    if (!isset($module_info[$module])) {
      $module_info[$module]['name'] = $module;
    }
    $options[$form_id] = t('!module: !form-title', array(
      '!form-title' => $info['title'],
      '!module' => t($module_info[$module]['name']),
    ));
  }
  // Sort form options by title.
  asort($options);

  return $options;
}

/**
 * Form builder; Configure Akismet protection for a form.
 */
function akismet_admin_configure_form($form, &$form_state, $akismet_form = NULL) {
  // If no $akismet_form was passed, then we are adding a new form configuration.
  if (!isset($akismet_form)) {
    if (!isset($form_state['storage']['akismet_form'])) {
      $form_state['storage']['step'] = 'select';
    }
    else {
      $form_state['storage']['step'] = 'configure';
      $akismet_form = $form_state['storage']['akismet_form'];
    }
  }
  // When adding a new form configuration, passing form_id via path argument.
  elseif (is_string($akismet_form)) {
    $akismet_form = akismet_form_new($akismet_form);
    $form_state['storage']['step'] = 'configure';
    $form_state['storage']['akismet_form'] = $akismet_form;
  }
  // Otherwise, we are editing an existing form configuration.
  else {
    $form_state['storage']['step'] = 'configure';
    $form_state['storage']['akismet_form'] = $akismet_form;
  }

  $form['#tree'] = TRUE;
  $form['actions'] = array(
    '#type' => 'actions',
  );
  $form['#attached'] = array(
    'js' => array(
      drupal_get_path('module', 'akismet') . '/akismet.admin.js',
    ),
  );

  switch ($form_state['storage']['step']) {
    case 'select':
      $form['akismet']['form_id'] = array(
        '#type' => 'select',
        '#title' => t('Form'),
        '#options' => akismet_admin_form_options(),
        '#required' => TRUE,
      );
      $form['actions']['next'] = array(
        '#type' => 'submit',
        '#value' => t('Next'),
        '#submit' => array('akismet_admin_configure_form_next_submit'),
      );
      break;

    case 'configure':
      drupal_set_title(t('Configure %form-title protection', array('%form-title' => $akismet_form['title'])), PASS_THROUGH);
      $recommended = t('recommended');

      $form['akismet']['form_id'] = array(
        '#type' => 'value',
        '#value' => $akismet_form['form_id'],
      );

      $modes = array();
      $modes[AKISMET_MODE_ANALYSIS] = t('!option <em>(!recommended)</em>', array(
        '!option' => t('Text analysis'),
        '!recommended' => $recommended,
      ));
      $modes[AKISMET_MODE_CAPTCHA] = t('CAPTCHA only');

      $form['akismet']['mode'] = array(
        '#type' => 'radios',
        '#title' => t('Protection mode'),
        '#options' => $modes,
        '#default_value' => isset($akismet_form['mode']) ? $akismet_form['mode'] : key($modes),
      );
      $form['akismet']['mode'][AKISMET_MODE_ANALYSIS] = array(
        '#description' => t('Akismet will analyze the post and will only show a CAPTCHA when it is unsure.'),
      );
      $form['akismet']['mode'][AKISMET_MODE_CAPTCHA] = array(
        '#description' => t('A CAPTCHA will be shown for every post. Only choose this if there are too few text fields to analyze.'),
      );
      $form['akismet']['mode'][AKISMET_MODE_CAPTCHA]['#description'] .= '<br />' . t('Note: Page caching is disabled on all pages containing a CAPTCHA-only protected form.');

      $all_permissions = array();
      foreach (module_implements('permission') as $module) {
        if ($module_permissions = module_invoke($module, 'permission')) {
          foreach ($module_permissions as &$info) {
            $info += array('module' => $module);
          }
          $all_permissions += $module_permissions;
        }
      }
      // Prepend Akismet's global permission to the list.
      array_unshift($akismet_form['bypass access'], 'bypass akismet protection');
      $permissions = array();
      foreach ($akismet_form['bypass access'] as $permission) {
        // @todo D7: Array keys are used as CSS class for the link list item,
        //   but are not sanitized: http://drupal.org/node/98696
        $permissions[drupal_html_class($permission)] = array(
          'title' => $all_permissions[$permission]['title'],
          'href' => 'admin/people/permissions',
          'fragment' => 'module-' . $all_permissions[$permission]['module'],
          'html' => TRUE,
        );
      }
      $form['akismet']['mode']['#description'] = t('The protection is omitted for users having any of the permissions: !permission-list', array(
        '!permission-list' => theme('links', array(
          'links' => $permissions,
          // @todo D7: Something went entirely wrong: system.menus.css makes ANY
          //   ul.links appear as if it would have the .inline CSS class.
          'attributes' => array(),
        )),
      ));

      // If not re-configuring an existing protection, make it the default.
      if (!isset($akismet_form['mode'])) {
        $form['akismet']['mode']['#default_value'] = AKISMET_MODE_ANALYSIS;
      }

      // Textual analysis filters.
      $form['akismet']['checks'] = array(
        '#type' => 'checkboxes',
        '#title' => t('Text analysis checks'),
        '#options' => array(
          'spam' => t('Spam'),
          'profanity' => t('Profanity'),
        ),
        '#default_value' => $akismet_form['checks'],
        '#states' => array(
          'visible' => array(
            ':input[name="akismet[mode]"]' => array('value' => (string) AKISMET_MODE_ANALYSIS),
          ),
        ),
      );
      // Profanity check requires text to analyze; unlike the spam check, there
      // is no fallback in case there is no text.
      $form['akismet']['checks']['profanity']['#access'] = !empty($akismet_form['elements']);

      // Form elements defined by hook_akismet_form_info() use the
      // 'parent][child' syntax, which Form API also uses internally for
      // form_set_error(), and which allows us to recurse into nested fields
      // during processing of submitted form values. However, since we are using
      // those keys also as internal values to configure the fields to use for
      // textual analysis, we need to encode them. Otherwise, a nested field key
      // would result in the following checkbox attribute:
      //   '#name' => 'akismet[enabled_fields][parent][child]'
      // This would lead to a form validation error, because it is a valid key.
      // By encoding them, we prevent this from happening:
      //   '#name' => 'akismet[enabled_fields][parent%5D%5Bchild]'
      $elements = array();
      foreach ($akismet_form['elements'] as $key => $value) {
        $elements[rawurlencode($key)] = $value;
      }
      $enabled_fields = array();
      foreach ($akismet_form['enabled_fields'] as $value) {
        $enabled_fields[] = rawurlencode($value);
      }
      $form['akismet']['enabled_fields'] = array(
        '#type' => 'checkboxes',
        '#title' => t('Text fields to analyze'),
        '#options' => $elements,
        '#default_value' => $enabled_fields,
        '#description' => t('Only enable fields that accept text (not numbers). Omit fields that contain sensitive data (e.g., credit card numbers) or computed/auto-generated values, as well as author information fields (e.g., name, e-mail).'),
        '#access' => !empty($akismet_form['elements']),
        '#states' => array(
          'visible' => array(
            ':input[name="akismet[mode]"]' => array('value' => (string) AKISMET_MODE_ANALYSIS),
          ),
        ),
      );

      $form['akismet']['unsure'] = array(
        '#type' => 'radios',
        // TODO: Rework this UI to better fit Akismet's wording: https://blog.akismet.com/2014/04/23/theres-a-ninja-in-your-akismet/
        '#title' => t('When Akismet thinks a post is spam, but is\'nt sure'),
        '#default_value' => $akismet_form['unsure'],
        '#options' => array(
//          'captcha' => t('!option <em>(!recommended)</em>', array(
//            '!option' => t('Show a CAPTCHA'),
//            '!recommended' => $recommended,
//          )),
          'moderate' => t('Retain the post for manual moderation'),
          'accept' => t('Accept the post'),
          'discard' => t('Disallow the post'),
        ),
        '#required' => $akismet_form['mode'] == AKISMET_MODE_ANALYSIS,
        // Only possible for forms protected via text analysis.
        '#states' => array(
          'visible' => array(
            ':input[name="akismet[mode]"]' => array('value' => (string) AKISMET_MODE_ANALYSIS),
            ':input[name="akismet[checks][spam]"]' => array('checked' => TRUE),
          ),
        ),
      );
      // Only possible for forms supporting moderation of unpublished posts.
      $form['akismet']['unsure']['moderate']['#access'] = !empty($akismet_form['moderation callback']);

      $form['akismet']['discard'] = array(
        '#type' => 'radios',
        '#title' => t('When text analysis identifies spam'),
        '#default_value' => $akismet_form['discard'],
        '#options' => array(
          1 => t('!option <em class="akismet-recommended">(!recommended)</em>', array(
            '!option' => t('Discard the post'),
            '!recommended' => $recommended,
          )),
          0 => t('!option <em class="akismet-recommended">(!recommended)</em>', array(
            '!option' => t('Retain the post for manual moderation'),
            '!recommended' => $recommended,
          )),
        ),
        '#required' => $akismet_form['mode'] == AKISMET_MODE_ANALYSIS,
        // Only possible for forms supporting moderation of unpublished posts.
        '#access' => !empty($akismet_form['moderation callback']),
        // Only possible for forms protected via text analysis.
        '#states' => array(
          'visible' => array(
            ':input[name="akismet[mode]"]' => array('value' => (string) AKISMET_MODE_ANALYSIS),
            ':input[name="akismet[checks][spam]"]' => array('checked' => TRUE),
          ),
        ),
      );

      // CMP integration requires Clean URLs to be enabled, in order for the
      // local moderation callback endpoints to work.
      $has_clean_urls = variable_get('clean_url', FALSE);

      $form['akismet']['moderation'] = array(
        '#type' => 'checkbox',
        '#title' => t('Allow content to be moderated from the <a href="@moderation-url">@moderation-product</a>', array(
          '@moderation-url' => 'https://akismet.com/moderation',
          '@moderation-product' => 'Akismet Content Moderation Platform',
        )),
        '#default_value' => (string) (int) ($akismet_form['moderation'] && $has_clean_urls),
        '#disabled' => !$has_clean_urls,
        // Only possible for forms which result in a locally stored entity.
        '#access' => !empty($akismet_form['entity']),
        // Only possible for forms protected via text analysis.
        '#states' => array(
          'visible' => array(
            ':input[name="akismet[mode]"]' => array('value' => (string) AKISMET_MODE_ANALYSIS),
          ),
        ),
        '#description' => t('Provides a unified moderation interface supporting multiple sites, moderation teams, and detailed analytics.'),
      );
      if (!$has_clean_urls) {
        $form['akismet']['moderation']['#description'] .= '<br />' . t('Requires <a href="@clean-urls">Clean URLs</a> to be enabled.', array(
          '@clean-urls' => url('admin/config/search/clean-urls'),
        ));
      }
      else {
        $form['akismet']['moderation']['#description'] .= '<br />' . t('Note: All content that was created while this option was disabled cannot be moderated from the @moderation-product; only new content will appear.', array(
          '@moderation-product' => 'Akismet Content Moderation Platform',
        ));
      }

      $form['actions']['submit'] = array(
        '#type' => 'submit',
        '#value' => t('Save'),
      );
      break;
  }

  $form['actions']['cancel'] = array(
    '#type' => 'link',
    '#title' => t('Cancel'),
    '#href' => 'admin/config/content/akismet',
  );

  return $form;
}

/**
 * Form submit handler for 'Next' button on Akismet form configuration form.
 */
function akismet_admin_configure_form_next_submit($form, &$form_state) {
  $form_id = $form_state['values']['akismet']['form_id'];
  $form_state['redirect'] = $_GET['q'] . '/' . $form_id;
}

/**
 * Form validation handler for akismet_admin_configure_form().
 */
function akismet_admin_configure_form_validate(&$form, &$form_state) {
  // For the 'configure' step, output custom #required form element errors for
  // 'checks' and 'enabled_fields', as their labels do not work with the default
  // #required form error message.
  if ($form_state['storage']['step'] == 'configure') {
    // Make field checkboxes required, if protection mode is text analysis.
    // @see http://drupal.org/node/875722
    $required = ($form_state['values']['akismet']['mode'] == AKISMET_MODE_ANALYSIS);
    $form['akismet']['checks']['#required'] = $required;
    $form['akismet']['discard']['#required'] = $required;

    if ($required && !array_filter($form_state['values']['akismet']['checks'])) {
      form_error($form['akismet']['checks'], t('At least one text analysis check is required.'));
    }
  }
}

/**
 * Form submit handler for akismet_admin_configure_form().
 */
function akismet_admin_configure_form_submit($form, &$form_state) {
  $akismet_form = $form_state['values']['akismet'];
  // Merge in form information from $form_state.
  $akismet_form += $form_state['storage']['akismet_form'];

  // Only store a list of enabled textual analysis checks.
  $akismet_form['checks'] = array_keys(array_filter($akismet_form['checks']));
  // Prepare selected fields for storage.
  $enabled_fields = array();
  foreach (array_keys(array_filter($akismet_form['enabled_fields'])) as $field) {
    $enabled_fields[] = rawurldecode($field);
  }
  $akismet_form['enabled_fields'] = $enabled_fields;

  $status = akismet_form_save($akismet_form);
  if ($status === SAVED_NEW) {
    drupal_set_message(t('The form protection has been added.'));
  }
  else {
    drupal_set_message(t('The form protection has been updated.'));
  }

  if (!empty($akismet_form['discard']) && !empty($akismet_form['moderation'])) {
    drupal_set_message(t('Spam that is discarded cannot be moderated from the Akismet Content Moderation Platform.'), 'warning');
  }
  else {
    $form_state['redirect'] = 'admin/config/content/akismet';
  }
}

/**
 * Form builder; Remove Akismet protection from a form.
 */
function akismet_admin_unprotect_form($form, &$form_state, $akismet_form) {
  $form['#tree'] = TRUE;
  $form['form'] = array(
    '#type' => 'item',
    '#title' => t('Form'),
    '#markup' => $akismet_form['title'],
  );
  $form['akismet']['form_id'] = array(
    '#type' => 'value',
    '#value' => $akismet_form['form_id'],
  );

  return confirm_form($form,
    t('Are you sure you want to unprotect this form?'),
    'admin/config/content/akismet',
    t('Akismet will no longer protect this form from spam.')
  );
}

/**
 * Form submit handler for akismet_admin_unprotect_form().
 */
function akismet_admin_unprotect_form_submit($form, &$form_state) {
  akismet_form_delete($form_state['values']['akismet']['form_id']);

  drupal_set_message(t('The form protection has been removed.'));

  $form_state['redirect'] = 'admin/config/content/akismet';
}

/**
 * Form constructor to configure the blacklist.
 *
 * @param $type
 *   The type of blacklist; i.e., 'spam', 'profanity', or 'unwanted'.
 */
function akismet_admin_blacklist_form($form, &$form_state, $type = 'spam') {
  $form['#tree'] = TRUE;
  $form['#attached']['css'][] = drupal_get_path('module', 'akismet') . '/akismet.admin.css';

  // Translate internal reason values for rendering and select list in form.
  $contexts = array(
    'allFields' => t('- All fields -'),
    'author' => t('- All author fields -'),
    'authorName' => t('Author name'),
    'authorMail' => t('Author e-mail'),
    'authorIp' => t('Author IP'),
    'authorId' => t('Author User ID'),
    'post' => t('- All post fields -'),
    'postTitle' => t('Post title'),
    'links' => t('Links'),
  );
  $matches = array(
    'contains' => t('contains'),
    'exact' => t('exact'),
  );

  $form['blacklist'] = array();
  // Do not retrieve the current blacklist when submitting the form.
  $blacklist = (empty($form_state['input']) ? akismet()->getBlacklist() : array());
  if (is_array($blacklist)) {
    foreach ($blacklist as $entry) {
      if ($entry['reason'] != $type) {
        continue;
      }
      $row = array();
      // #class property is internally used by
      // theme_akismet_admin_blacklist_form().
      $row['context'] = array(
        '#markup' => check_plain(isset($contexts[$entry['context']]) ? $contexts[$entry['context']] : $entry['context']),
        '#class' => 'akismet-blacklist-context value-' . check_plain($entry['context']),
      );
      $row['match'] = array(
        '#markup' => check_plain($matches[$entry['match']]),
        '#class' => 'akismet-blacklist-match value-' . check_plain($entry['match']),
      );
      $row['value'] = array(
        '#markup' => check_plain($entry['value']),
        '#class' => 'akismet-blacklist-value',
      );
      $row['actions']['delete'] = array(
        '#type' => 'link',
        '#title' => t('delete'),
        '#href' => 'admin/config/content/akismet/blacklist/delete',
        '#options' => array(
          'query' => array('id' => $entry['id']) + drupal_get_destination(),
        ),
      );
      $form['blacklist'][$entry['id']] = $row;
    }
  }

  $form['entry']['context'] = array(
    '#type' => 'select',
    '#title' => t('Context'),
    '#title_display' => 'invisible',
    '#options' => $contexts,
    '#required' => TRUE,
    '#id' => 'akismet-blacklist-filter-context',
  );
  $form['entry']['match'] = array(
    '#type' => 'select',
    '#title' => t('Match'),
    '#title_display' => 'invisible',
    '#options' => $matches,
    '#required' => TRUE,
    '#id' => 'akismet-blacklist-filter-match',
  );
  $form['entry']['value'] = array(
    '#type' => 'textfield',
    '#title' => t('Value'),
    '#title_display' => 'invisible',
    '#size' => 40,
    '#required' => TRUE,
    '#maxlength' => 64,
    '#id' => 'akismet-blacklist-filter-value',
    '#attributes' => array(
      'autocomplete' => 'off',
    ),
  );
  $form['entry']['reason'] = array(
    '#type' => 'value',
    '#value' => $type,
  );
  $form['entry']['actions'] = array(
    '#type' => 'actions',
    '#tree' => FALSE,
  );
  $form['entry']['actions']['submit'] = array(
    '#type' => 'submit',
    '#value' => t('Add'),
  );

  return $form;
}

/**
 * Form submit handler to save a text string to the Akismet blacklist.
 */
function akismet_admin_blacklist_form_submit($form, &$form_state) {
  $data = array(
    'value' => $form_state['values']['entry']['value'],
    'context' => $form_state['values']['entry']['context'],
    'match' => $form_state['values']['entry']['match'],
    'reason' => $form_state['values']['entry']['reason'],
  );
  $result = akismet()->saveBlacklistEntry($data);

  $args = array(
    '@value' => $data['value'],
    '@context' => $data['context'],
    '@match' => $data['match'],
    '@reason' => $data['reason'],
  );
  if (!empty($result['id'])) {
    drupal_set_message(t('The entry was added to the blacklist.'));
    akismet_log(array(
      'message' => 'Added @value (@context, @match) to @reason blacklist.',
      'arguments' => $args,
    ));
  }
  else {
    drupal_set_message(t('An error occurred upon trying to add the value to the blacklist.'), 'error');
    akismet_log(array(
      'message' => 'Failed to add @value (@context, @match) to @reason blacklist.',
      'arguments' => $args,
    ), WATCHDOG_ERROR);
  }
}

/**
 * Formats the blacklist form as table to embed the form.
 */
function theme_akismet_admin_blacklist_form($variables) {
  $form = $variables['form'];
  $header = array(
    t('Context'),
    t('Matches'),
    t('Value'),
    '',
  );
  $rows = array();

  $rows[] = array(
    drupal_render($form['entry']['context']),
    drupal_render($form['entry']['match']),
    drupal_render($form['entry']['value']),
    drupal_render($form['entry']['actions']),
  );

  foreach (element_children($form['blacklist']) as $id) {
    $rows[] = array(
      array(
        'data' => drupal_render($form['blacklist'][$id]['context']),
        'class' => $form['blacklist'][$id]['context']['#class'],
      ),
      array(
        'data' => drupal_render($form['blacklist'][$id]['match']),
        'class' => $form['blacklist'][$id]['match']['#class'],
      ),
      array(
        'data' => drupal_render($form['blacklist'][$id]['value']),
        'class' => $form['blacklist'][$id]['value']['#class'],
      ),
      drupal_render($form['blacklist'][$id]['actions']),
    );
  }

  // This table is never empty due to the form.
  $output  = theme('table', array('header' => $header, 'rows' => $rows, 'attributes' => array('id' => 'akismet-blacklist')));
  $output .= drupal_render_children($form);

  drupal_add_js(drupal_get_path('module', 'akismet') . '/akismet.admin.blacklist.js');

  return $output;
}

/**
 * Form builder; Builds the confirmation form for deleting a blacklist item.
 *
 * @ingroup forms
 * @see akismet_admin_blacklist_delete_submit()
 */
function akismet_admin_blacklist_delete($form, &$form_state) {
  $id = $_GET['id'];
  if (empty($id) || !($entry = akismet()->getBlacklistEntry($id))) {
    drupal_not_found();
    return;
  }
  $form['entry'] = array(
    '#type' => 'value',
    '#value' => $entry,
  );

  return confirm_form(
    $form,
    t('Are you sure you want to delete %value from the blacklist?', array('%value' => $entry['value'])),
    'admin/config/content/akismet/blacklist',
    t('This action cannot be undone.'),
    t('Delete'), t('Cancel')
  );
}

/**
 * Form submit handler to delete an entry from the blacklist.
 */
function akismet_admin_blacklist_delete_submit($form, &$form_state) {
  $result = akismet()->deleteBlacklistEntry($form_state['values']['entry']['id']);

  $args = array(
    '@value' => $form_state['values']['entry']['value'],
    '@context' => $form_state['values']['entry']['context'],
    '@reason' => $form_state['values']['entry']['reason'],
  );
  if ($result === TRUE) {
    drupal_set_message(t('The entry was removed from the blacklist.'));
    akismet_log(array(
      'message' => 'Removed @value (@context) from @reason blacklist.',
      'arguments' => $args,
    ));
  }
  else {
    drupal_set_message(t('An error occurred upon trying to remove the item from the blacklist.'), 'error');
    akismet_log(array(
      'message' => 'Failed to remove @value (%context) from @reason blacklist.',
      'arguments' => $args,
    ), WATCHDOG_ERROR);
  }

  $form_state['redirect'] = 'admin/config/content/akismet/blacklist';
}

/**
 * Form builder; Global Akismet settings form.
 *
 * This form does not validate Akismet API keys, since the fallback method still
 * needs to be able to be reconfigured in case Akismet services are down.
 * akismet.verifyKey would invalidate the keys and throw an error; hence,
 * _akismet_fallback() would invoke form_set_error(), effectively preventing this
 * form from submitting.
 *
 * @todo Implement proper form validation now that akismet() no longer triggers
 *   the fallback mode.
 */
function akismet_admin_settings($form, &$form_state) {
  $akismet = akismet();
  $check = empty($_POST);
  $status = akismet_admin_site_status($check);
  if ($check && $status['isVerified'] && !variable_get('akismet_testing_mode', 0)) {
    drupal_set_message(t('Akismet servers verified your keys. The services are operating correctly.'));
  }

  $form['access-keys'] = array(
    '#type' => 'fieldset',
    '#title' => t('Akismet API keys'),
    '#description' => t('To obtain API keys, <a href="@signup-url">sign up</a> or log in to your <a href="@site-manager-url">Site manager</a>, register this site, and copy the keys into the fields below.', array(
      '@signup-url' => 'https://akismet.com/pricing',
      '@site-manager-url' => 'https://akismet.com/site-manager',
    )),
    '#collapsible' => TRUE,
    // Only show key configuration fields if they are not configured or invalid.
    '#collapsed' => !$status['isVerified'],
  );
  // Keys are not #required to allow to install this module and configure it
  // later.
  $form['access-keys']['akismet_public_key'] = array(
    '#type' => 'textfield',
    '#title' => t('Public key'),
    '#default_value' => variable_get('akismet_public_key'),
    '#element_validate' => array('akismet_admin_settings_validate_key'),
    '#description' => t('Used to uniquely identify this site.'),
  );
  $form['access-keys']['akismet_private_key'] = array(
    '#type' => 'textfield',
    '#title' => t('Private key'),
    '#default_value' => variable_get('akismet_private_key'),
    '#element_validate' => array('akismet_admin_settings_validate_key'),
    '#description' => t('Used for authentication. Similar to a password, the private key should not be shared with anyone.'),
  );
  $form['access-keys']['ping_holder'] = array(
    '#type' => 'fieldset',
    '#title' => t('Test connection'),
    '#title_display' => 'invisible',
    '#states' => array(
      'visible' => array(
        ':input[name="akismet_public_key"]' => array('!value' => ''),
        ':input[name="akismet_private_key"]' => array('!value' => ''),
      ),
    ),
    'ping_action' => array(
      '#type' => 'button',
      '#value' => t('Ping Akismet'),
      '#submit' => array('akismet_admin_ping_submit'),
      '#ajax' => array(
        'callback' => 'akismet_admin_ping_ajax_callback',
        'wrapper' => 'akismet-ping-response',
        'method' => 'replace',
        'effect' => 'fade',
      ),
    ),
    'help' => array(
      '#type' => 'container',
      '#attributes' => array(
        'class' => array('description'),
      ),
      'helptext' => array(
        '#markup' => t('Be sure that you have checked "Log all Akismet messages" in the "Advanced" configuration in order to review full API responses.'),
      ),
    ),
    'akismet_ping_replace' => array(
      '#type' => 'container',
      '#attributes' => array(
        'id' => 'akismet-ping-response',
      ),
      'response' => array(
        '#markup' => isset($form_state['storage']['akismet_ping']) ? $form_state['storage']['akismet_ping'] : '',
      ),
    )
  );
  $form['akismet_fallback'] = array(
    '#type' => 'radios',
    '#title' => t('When the Akismet service is unavailable'),
    '#default_value' => variable_get('akismet_fallback', AKISMET_FALLBACK_ACCEPT),
    '#options' => array(
      AKISMET_FALLBACK_ACCEPT => t('Accept all form submissions'),
      AKISMET_FALLBACK_BLOCK => t('Block all form submissions'),
    ),
    '#description' => t('Akismet offers a <a href="@pricing-url">high-availability</a> infrastructure for users on paid plans to reduce potential downtime.', array(
      '@pricing-url' => 'https://akismet.com/pricing',
    )),
  );

  $options = _akismet_supported_languages();
  $default_languages = array();
  if (isset($status['expectedLanguages'])) {
    $default_languages = $status['expectedLanguages'];
  }
  else {
    $default_languages = $akismet->loadConfiguration('expectedLanguages');
  }
  $path = drupal_get_path('module', 'akismet');
  $form[$akismet->configuration_map['expectedLanguages']] = array(
    '#type' => 'select',
    '#title' => t('Expected languages'),
    '#options' => $options,
    '#multiple' => TRUE,
    '#size' => 6,
    '#default_value' => $default_languages,
    '#description' => t('Restricts all posts to selected languages. Used by text analysis only. Leave empty if users may post in other languages.'),
    // Ensure that selected languages are apparent for site administrators and
    // not potentially hidden in a large select widget.
    '#attributes' => array(
      // @see form_process_select()
      'data-placeholder' => t('- Empty -'),
      'id' => $akismet->configuration_map['expectedLanguages'],
    ),
    '#attached' => array(
      'js' => array(
        $path . '/akismet.admin.js',
      ),
    ),
  );

  // Add the chosen library if available through the libraries module.
  if (module_exists('libraries')) {
    if ($path = libraries_get_path('chosen')) {
      $form[$akismet->configuration_map['expectedLanguages']]['#attached']['js'][] = $path . '/chosen.jquery.min.js';
      $form[$akismet->configuration_map['expectedLanguages']]['#attached']['css'][] = $path . '/chosen.min.css';
    }
  }

  $form['akismet_privacy_link'] = array(
    '#type' => 'checkbox',
    '#title' => t("Show a link to Akismet's privacy policy"),
    '#return_value' => 1,
    '#default_value' => variable_get('akismet_privacy_link', 1),
    '#description' => t('Only applies to forms protected with text analysis. When disabling this option, you should inform visitors about the privacy of their data through other means.'),
  );

  $form['akismet_testing_mode'] = array(
    '#type' => 'checkbox',
    '#title' => t('Enable testing mode'),
    '#default_value' => variable_get('akismet_testing_mode', 0),
    '#description' => t('Submitting "ham", "unsure", or "spam" triggers the corresponding behavior; image CAPTCHAs only respond to "correct" and audio CAPTCHAs only respond to "demo". Do not enable this option if this site is publicly accessible.'),
  );

  $form['akismet_advanced'] = array(
    '#type' => 'fieldset',
    '#title' => t('Advanced configuration'),
    '#collapsible' => TRUE,
    '#collapsed' => TRUE,
  );
  // Lower severity numbers indicate a high severity level.
  $min_severity = variable_get('akismet_log_minimum_severity', WATCHDOG_WARNING);
  $form['akismet_advanced']['akismet_log_minimum_severity'] = array(
    '#type' => 'radios',
    '#title' => t('Akismet logging level warning'),
    '#options' => array(
      WATCHDOG_WARNING => t('Only log warnings and errors'),
      WATCHDOG_DEBUG => t('Log all Akismet messages'),
    ),
    '#default_value' => $min_severity <= WATCHDOG_WARNING ? WATCHDOG_WARNING : WATCHDOG_DEBUG,
  );
  $form['akismet_advanced']['akismet_audio_captcha_enabled'] = array(
    '#type' => 'checkbox',
    '#title' => t('Enable audio CAPTCHAs.'),
    '#description' => t('Allows users to switch to an audio verification using the <a href="!faq-url">NATO alphabet</a>.  This may not be appropriate for non-English language sites.', array(
      '!faq-url' => 'https://akismet.com/faq/akismet-audible-captcha-language',
    )),
    '#default_value' => variable_get('akismet_audio_captcha_enabled', 1),
  );
  $form['akismet_advanced']['akismet_connection_timeout'] = array(
    '#type' => 'textfield',
    '#title' => t('Time-out when attempting to contact Akismet servers.'),
    '#description' => t('This is the length of time that a call to Akismet will wait before timing out.'),
    '#default_value' => variable_get('akismet_connection_timeout', 3),
    '#size' => 5,
    '#field_suffix' => t('seconds'),
    '#required' => TRUE,
  );
  $form['akismet_advanced']['akismet_fba_enabled'] = array(
    '#type' => 'checkbox',
    '#title' => t('Enable form behavior analysis (beta).'),
    '#description' => t('This will place a small tracking image from Akismet on each form protected by textural analysis to help Akismet determine if the form is filled out by a bot.  <a href="!fba-url">Learn more</a>.', array(
      '!fba-url' => 'https://akismet.com/faq/form-behavior-analysis',
    )),
    '#default_value' => variable_get('akismet_fba_enabled', 0),
  );
  // Available entity types are those entities that have a report access
  // callback defined.  This is limited by type even though multiple forms
  // can be for the same entity type.
  $forms = akismet_form_list();
  $options = array();
  foreach($forms as $info) {
    if (!empty($info['entity']) && !empty($info['entity report access callback'])) {
      $options[] = $info['entity'];
    }
  };
  sort($options);
  $form['akismet_advanced']['akismet_fai_entity_types'] = array(
    '#type' => 'checkboxes',
    '#title' => t('Allow users to  "Flag as Inappropriate" for the following:'),
    '#description' => t('"Flag as inappropriate" will only appear on protected forms for users who have permission to the content type and have the permission to "Report to Akismet".  <a href="!fai-url">Learn more</a>.', array(
      '!fai-url' => 'https://akismet.com/faq/flag-as-inappropriate',
    )),
    '#options' => drupal_map_assoc($options),
    '#default_value' => variable_get('akismet_fai_entity_types', array('comment' => 'comment')),
  );
  // Only display dialog options if the user has at least one integrated
  // module installed and enabled.
  module_load_include('inc', 'akismet', 'akismet.flag');
  $implemented = akismet_flag_dialog_info();
  $available = array_intersect_key($implemented, module_list());
  $options = array();
  foreach($available as $module => $info) {
    $options[$module] = $info['title'];
  }
  if (count($options) > 0) {
    $options['akismet'] = 'Akismet custom dialog';
    $form['akismet_advanced']['akismet_fai_dialog'] = array(
      '#type' => 'radios',
      '#title' => t('Flag as inappropriate dialog type'),
      '#options' => $options,
      '#default_value' => variable_get('akismet_fai_dialog', 'akismet'),
    );
  }
  else {
    // Reset to use the Akismet dialog if no integrated modules are available.
    $form['akismet_advanced']['akismet_fai_dialog'] = array(
      '#type' => 'value',
      '#value' => 'akismet',
    );
  }

  $form['#submit'][] = 'akismet_admin_settings_prepare_submit';
  $form = system_settings_form($form);
  $form['#submit'][] = 'akismet_admin_settings_submit';
  return $form;
}

/**
 * Element validation handler for API key text fields.
 */
function akismet_admin_settings_validate_key($element, &$form_state) {
  if ($element['#value'] !== '') {
    // Remove any leading/trailing white-space and override submitted value.
    $element['#value'] = trim($element['#value']);
    form_set_value($element, $element['#value'], $form_state);
    // Verify the key has a length of 32 characters.
    if (drupal_strlen($element['#value']) != 32) {
      form_error($element, t('!title must be 32 characters. Ensure you copied the key correctly.', array(
        '!title' => $element['#title'],
      )));
    }
  }
}

/**
 * Form submission handler for global settings form.
 */
function akismet_admin_settings_prepare_submit($form, &$form_state) {
  // If the minimum log severity checkbox was disabled (no input), convert
  // 0 into WATCHDOG_DEBUG.
  if (!isset($form_state['input']['akismet_log_minimum_severity'])) {
    $form_state['values']['akismet_log_minimum_severity'] = WATCHDOG_DEBUG;
  }
}

/**
 * Form submission handler for global settings form.
 */
function akismet_admin_settings_submit($form, &$form_state) {
  // Update Akismet site record with local configuration.
  _akismet_status(TRUE, TRUE);
}

/**
 * Form submit handler for the ping button.
 */
function akismet_admin_ping_ajax_callback($form, &$form_state) {
  $akismet = akismet();
  $response = $akismet->pingAPI($form_state['values']['akismet_public_key'], $form_state['values']['akismet_private_key']);
  $code = isset($response['code']) ? $response['code'] : $response;
  if (drupal_strlen($code)) {
    $log_message = t('Akismet API response status: @response.  Please check your <a href="!log_url">Recent log messages</a> for details.', array(
      '@response' => $code,
      '!log_url' => url('admin/reports/dblog'),
    ));
  }
  else {
    $log_message = t('Ping request succeeded.  Please check your <a href="!log_url">Recent log messages</a> for pong callback.', array(
      '!log_url' => url('admin/reports/dblog'),
    ));
  }
  $form_state['storage']['akismet_ping'] = $form['access-keys']['ping_holder']['akismet_ping_replace']['response']['#markup'] = $log_message;
  return $form['access-keys']['ping_holder']['akismet_ping_replace'];
}

/**
 * Callback for Akismet API pong return.
 *
 * NOTE:  This function does not have any access restrictions due to the nature
 * of an API callback.  Therefore, no additional work should be performed here.
 */
function akismet_admin_pong_callback() {
  if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    akismet_log(array(
      'message' => 'Akismet API pong called using invalid request method',
    ), WATCHDOG_ERROR);
    return MENU_NOT_FOUND;
  }
  akismet_log(array(
    'message' => 'Akismet API: pong',
  ), WATCHDOG_INFO);
  // Return empty content so that Akismet receives an appropriate HTTP status
  // code.
  return array();
}


/**
 * Menu callback; Displays the administrative reports page.
 */
function akismet_reports_page($form, &$form_state) {
  $embed_attributes = array(
    'src' => 'https://akismet.com/statistics.swf?key=' . check_plain(variable_get('akismet_public_key', '')),
    'quality' => 'high',
    'width' => '100%',
    'height' => '430',
    'name' => 'Akismet',
    'align' => 'middle',
    'play' => 'true',
    'loop' => 'false',
    'allowScriptAccess' => 'sameDomain',
    'type' => 'application/x-shockwave-flash',
    'pluginspage' => 'http://www.adobe.com/go/getflashplayer',
    'wmode' => 'transparent',
  );
  $form['chart'] = array(
    '#type' => 'item',
    '#title' => t('Statistics'),
    '#markup' => '<embed' . drupal_attributes($embed_attributes) . '></embed>',
  );

  return $form;
}

