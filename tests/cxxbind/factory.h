#pragma once

#include <cstdint>
#include <memory>

// A C++-shaped library for the factory/ownership slice of the bridge. The
// public surface is deliberately close to Skia: a factory-only Surface, a
// borrowed Canvas, an owned Image snapshot, a value Rect and a refcount-style
// Box created by a static factory.
struct Rect {
  static Rect make(float l, float t, float r, float b);
  float left() const;
  float top() const;
  float right() const;
  float bottom() const;

  float l;
  float t;
  float r;
  float b;
};

class Box {
 public:
  static std::unique_ptr<Box> make(int w, int h);
  ~Box();
  int width() const;
  int height() const;
  Box* clone() const;

 private:
  Box(int w, int h);
  int w_;
  int h_;
};

class Canvas {
 public:
  Canvas();
  ~Canvas();
  void clear(uint32_t color);
  void drawRect(const Rect& rect, uint32_t color);
  void drawBox(const Box& box);
  int count() const;

 private:
  int count_ = 0;
  uint32_t color_ = 0;
};

class Image {
 public:
  Image() = default;
  ~Image() = default;
};

class Ref {
 public:
  static std::unique_ptr<Ref> make(int value);
  ~Ref();
  void unref();
  int value() const;

 private:
  explicit Ref(int value);
  int value_;
  int refs_ = 1;
};

class Surface {
 public:
  static std::unique_ptr<Surface> make(int w, int h);
  ~Surface();
  Canvas* canvas();
  std::unique_ptr<Image> snapshot();

 private:
  Surface(int w, int h);
  Canvas canvas_;
  int w_;
  int h_;
};
