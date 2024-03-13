# Navigation TextureRender View Plugin

Two modes are supported:
* AndroidView
  * view name `views/nav-render-view`
  * view_type
  * Implemented as a Wayland Compositor Sub Surface
  
* platform channel
  * channel name `nav_render_view`
  * Implements a GL Texture
  * Requires GL backend
