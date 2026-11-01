<script>
  // The long tail's widgets («Інші параметри», lib/longtail.js): the generic ones a module's new key
  // may come with. The framework's own (Wi-Fi, time, firmware, access, the AP) are not here —
  // their keys are the hand-written pages', so the tail never shows them.
  import ValueWidget from './widgets/ValueWidget.svelte';
  import SliderWidget from './widgets/SliderWidget.svelte';
  import NumberInput from './widgets/NumberInput.svelte';
  import IndicatorWidget from './widgets/IndicatorWidget.svelte';
  import StatusText from './widgets/StatusText.svelte';
  import ToggleWidget from './widgets/ToggleWidget.svelte';
  import TextInput from './widgets/TextInput.svelte';
  import PasswordInput from './widgets/PasswordInput.svelte';
  import ButtonWidget from './widgets/ButtonWidget.svelte';
  import MqttSave from './widgets/MqttSave.svelte';
  import SelectWidget from './widgets/SelectWidget.svelte';

  export let widget;
  export let value;

  const widgetMap = {
    value: ValueWidget,
    slider: SliderWidget,
    number_input: NumberInput,
    indicator: IndicatorWidget,
    status_text: StatusText,
    toggle: ToggleWidget,
    text_input: TextInput,
    password_input: PasswordInput,
    button: ButtonWidget,
    mqtt_save: MqttSave,
    select: SelectWidget
  };

  $: component = widgetMap[widget.widget] || ValueWidget;
</script>

<div class="widget-wrapper" data-widget-key={widget.key}>
  <svelte:component this={component} config={widget} {value} />
</div>

<style>
  .widget-wrapper {
    margin-bottom: var(--sp-3);
    min-height: var(--touch-min);
    display: flex;
    flex-direction: column;
    justify-content: center;
  }
  .widget-wrapper:last-child {
    margin-bottom: 0;
  }
</style>
