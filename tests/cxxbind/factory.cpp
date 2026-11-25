#include "factory.h"

Rect Rect::make(float l, float t, float r, float b) { return Rect{l, t, r, b}; }
float Rect::left() const { return l; }
float Rect::top() const { return t; }
float Rect::right() const { return r; }
float Rect::bottom() const { return b; }

Box::Box(int w, int h) : w_(w), h_(h) {}
Box::~Box() = default;
std::unique_ptr<Box> Box::make(int w, int h) { return std::unique_ptr<Box>(new Box(w, h)); }
int Box::width() const { return w_; }
int Box::height() const { return h_; }
Box* Box::clone() const { return new Box(w_, h_); }

Canvas::Canvas() = default;
Canvas::~Canvas() = default;
void Canvas::clear(uint32_t color) { color_ = color; }
void Canvas::drawRect(const Rect&, uint32_t color) {
  ++count_;
  color_ = color;
}
void Canvas::drawBox(const Box&) { ++count_; }
int Canvas::count() const { return count_; }

Surface::Surface(int w, int h) : w_(w), h_(h) {
  // The fixture only needs a class with private state; nothing reads the size.
  (void)w_;
  (void)h_;
}
Surface::~Surface() = default;
std::unique_ptr<Surface> Surface::make(int w, int h) {
  return std::unique_ptr<Surface>(new Surface(w, h));
}
Canvas* Surface::canvas() { return &canvas_; }
std::unique_ptr<Image> Surface::snapshot() { return std::make_unique<Image>(); }

Ref::Ref(int value) : value_(value) {}
Ref::~Ref() = default;
std::unique_ptr<Ref> Ref::make(int value) {
  return std::unique_ptr<Ref>(new Ref(value));
}
void Ref::unref() {
  if (--refs_ == 0) delete this;
}
int Ref::value() const { return value_; }
