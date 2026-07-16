{% extends '//die/hub.sh' %}

{% block run_deps %}
# Pyrefly enables tikv-jemallocator,
# whose FFI requires jemalloc-specific symbols.
# rust_devendor disables tikv-jemalloc-sys's build script,
# so IX must provide jemalloc explicitly;
# another allocator leaves those symbols unresolved
# and the final link fails.
ynd/bin/pyrefly/unwrap(force_allocator=jemalloc)
{% endblock %}
